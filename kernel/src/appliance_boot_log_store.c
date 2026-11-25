#include "appliance_boot_log_store.h"

#include <stddef.h>
#include <string.h>

#include "appliance_gpt_discovery.h"
#include "arch/x86_64/timer.h"
#include "efi_boot_environment.h"

/*
 * ITEM-410: the appliance writes its own boot log to its own disk.
 *
 * The records this kernel publishes about an install, the network, the local
 * input path, its readiness and its refusals are already mirrored on the
 * framebuffer console (ITEM-405) and kept in a bounded RAM ring
 * (`appliance_runtime_log`), but a hardware run ends with photographs of a
 * 16-line screen: one record scrolls away and costs an iteration, and a machine
 * whose run refuses before the console comes up shows nothing at all.
 *
 * This module is the durable half.  It stages every published record in a
 * bounded RAM ring as it appears, and writes the ring into a preallocated file
 * of ours on the EFI System Partition at the moments that matter - readiness,
 * DHCP, the serving path, every refusal or failure, and the panic path.  The
 * file is written so that any Linux can read the result with a plain mount, and
 * `scripts/hfoc-esp-read.py --bootlog` reads the same bytes out of a raw image
 * with sequence numbers attached.
 *
 * Everything here is best effort by construction: no function in this module
 * returns a failure a boot has to care about, a failed flush leaves the records
 * staged for the next attempt, and the failure itself is published as a record
 * so the operator sees `state=UNAVAILABLE` instead of a silently missing log.
 */

/*
 * ITEM-481: the line buffer holds the worst case of the `[BOOTLOG] state=READY`
 * record (twelve decimal fields at twenty digits, the path, the literals and a
 * long reason), not the console record bound, so the log's own bookkeeping
 * cannot drop a field before the bounded console funnel splits it into
 * `[BOOTLOG-TAIL]` parts.
 */
#define APPLIANCE_BOOT_LOG_STORE_LINE_MAX 640U
#define APPLIANCE_BOOT_LOG_STORE_RECORD_MAX 400U
/*
 * ITEM-518: how many staged records the console dump prints before it publishes
 * a progress line.  The window itself is bounded by the staging ring
 * (`APPLIANCE_BOOT_LOG_PENDING_CAPACITY`), so one `bootlog` is a fixed amount of
 * work; the chunk line is what shows a person that the work is still moving.
 */
#define APPLIANCE_BOOT_LOG_DUMP_CHUNK 16U
#define APPLIANCE_BOOT_LOG_STORE_SCAVENGE_CHUNK \
    (APPLIANCE_BOOT_LOG_SLOT_SIZE * 8U)
/*
 * ITEM-497: how far back the attach looks for the verdicts earlier boots
 * wrote, and how often a boot re-publishes them.  The tail is scanned because a
 * verdict outlives the records around it only while it is refreshed: the ring
 * holds 511 records of a boot that publishes thousands, so a boot's own verdict
 * is re-stated (with the two earlier boots it carries) every digest window, and
 * the window is well inside what the ring keeps.
 */
#define APPLIANCE_BOOT_LOG_STORE_VERDICT_SCAN_SLOTS 256U
#define APPLIANCE_BOOT_LOG_STORE_VERDICT_CHUNK_SLOTS 16U
#define APPLIANCE_BOOT_LOG_STORE_VERDICT_DIGEST_TICKS (10U * HFOC_TIMER_HZ)
/*
 * The periodic record families the durable ring does not need in every copy.
 *
 * Two rules, because the two kinds of family are not the same:
 *
 *   * the log's own bookkeeping (`[BOOTLOG] state=READY`, `[BOOTLOG-FLUSH`)
 *     carries counters that change with every flush, so identity says nothing
 *     about it; it is admitted at most once per interval, and the first record
 *     of the boot is always admitted;
 *   * the per-second snapshots (`[INPUT-DIAG]`, `[LOOP-DIAG]`, `[XHCI-DIAG]`)
 *     ARE their content: a record identical to the last admitted one of its
 *     family is a repeat of an unchanged state and is held back, but a record
 *     that changed - a counter moved, a role became ready - is published at
 *     once, and a family that stays unchanged is still restated once per
 *     interval.  That is what keeps a live controller's evidence per-second
 *     while a controller with no device stops repeating itself.
 *
 * The families that carry a state change - a refusal, the input watchdog, the
 * mouse silence snapshots, the iteration maximum - are not here at all and are
 * written the moment they happen.
 */
#define APPLIANCE_BOOT_LOG_STORE_GATE_FAMILIES 5U
/* The two families whose content changes every time they are published. */
#define APPLIANCE_BOOT_LOG_STORE_GATE_BY_TIME 3U
static const char *const appliance_boot_log_store_gate_markers[
    APPLIANCE_BOOT_LOG_STORE_GATE_FAMILIES] = {
    "[INPUT-DIAG]",
    "[LOOP-DIAG]",
    "[XHCI-DIAG]",
    "[BOOTLOG] state=READY",
    "[BOOTLOG-FLUSH",
};
static const uint64_t appliance_boot_log_store_gate_intervals[
    APPLIANCE_BOOT_LOG_STORE_GATE_FAMILIES] = {
    10U * HFOC_TIMER_HZ,
    10U * HFOC_TIMER_HZ,
    10U * HFOC_TIMER_HZ,
    30U * HFOC_TIMER_HZ,
    30U * HFOC_TIMER_HZ,
};

struct appliance_boot_log_store {
    bool initialized;
    bool bound;
    bool header_written;
    bool unavailable_published;
    bool in_flush;
    bool panic;
    /*
     * ITEM-518: true while `appliance_boot_log_store_dump()` is publishing its
     * own output.  The publish sink reaches the console journal, the journal
     * hands the record back to `observe`, and an unguarded observe appended the
     * dump's own lines to the ring - which moved the end of the loop the dump
     * was iterating and made `bootlog` run forever.  The dump's output is a
     * reading of the log, not a record of the boot, and is not observed.  A
     * record published by an interrupt while the reading runs is not staged
     * either: the reading owns the ring for its duration, and a bounded reading
     * that misses one interrupt-time record beats an unbounded one that never
     * returns.
     */
    bool dumping;
    enum appliance_boot_log_store_status status;
    const char *failure_reason;
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    uint64_t esp_first_lba;
    uint64_t esp_sectors;
    bool reused;
    struct appliance_boot_log_header header;
    struct appliance_boot_log_ring ring;
    bool dedup_valid;
    size_t dedup_length;
    char dedup[APPLIANCE_BOOT_LOG_TEXT_MAX + 1U];
    /* ITEM-497: the repeat gate, the boot's own input verdict, the earlier
     * boots it carries, and when the verdict set was last re-published. */
    struct appliance_boot_log_gate gate;
    /* The last record admitted per family, so a repeat of an unchanged state
     * is recognised instead of being written again. */
    bool gate_last_valid[APPLIANCE_BOOT_LOG_STORE_GATE_FAMILIES];
    size_t gate_last_length[APPLIANCE_BOOT_LOG_STORE_GATE_FAMILIES];
    char gate_last[APPLIANCE_BOOT_LOG_STORE_GATE_FAMILIES]
                  [APPLIANCE_BOOT_LOG_TEXT_MAX + 1U];
    bool input_valid;
    size_t input_length;
    char input[APPLIANCE_BOOT_LOG_INPUT_BODY_MAX + 1U];
    size_t carried_count;
    struct appliance_boot_log_input_record carried[
        APPLIANCE_BOOT_LOG_INPUT_CARRY_MAX];
    uint64_t next_boot;
    uint64_t digest_ticks;
    struct appliance_boot_log_store_sink sink;
};

static struct appliance_boot_log_store store;

/*
 * ITEM-497: the input verdict's durable half.  The scan runs once, at attach,
 * over the tail of the extent the previous boots wrote; the digest re-publishes
 * what it found, so a verdict survives the boots that follow it.
 */
static void appliance_boot_log_store_load_verdicts(void);
static void appliance_boot_log_store_input_digest(void);
void appliance_boot_log_store_input_verdict_tick(void);
static bool appliance_boot_log_store_gate_admit(
    const char *record,
    size_t length,
    uint64_t ticks);

/*
 * ITEM-487: the run of slots one file-range write carries.  The pending ring
 * holds at most this many records, so a flush that the file's own ring does not
 * split is one range write; the buffer is a static, not a stack object, because
 * 32 KiB has no business on the boot stack.
 */
static unsigned char store_flush_batch[
    APPLIANCE_BOOT_LOG_PENDING_CAPACITY * APPLIANCE_BOOT_LOG_SLOT_SIZE];

/*
 * ITEM-487: the reading `appliance_boot_log_store_last_flush()` hands back.
 * One bounded struct filled by the flush that just finished; no path reads it.
 */
static struct appliance_boot_log_flush_stats store_flush_stats;

/* ------------------------------------------------------------ the sink ---- */

static void appliance_boot_log_store_publish(const char *text)
{
    if (store.sink.publish == NULL || text == NULL) {
        return;
    }
    /*
     * A publication paints the framebuffer console as well as the serial
     * stream.  On the panic path the screen belongs to the panic message: the
     * one thing the log does there is write what it already has, and it
     * publishes nothing that could paint over the reason the machine stopped.
     */
    if (store.panic) {
        return;
    }
    store.sink.publish(text, strlen(text));
}

static void appliance_boot_log_store_print(const char *text)
{
    if (store.sink.print == NULL || text == NULL) {
        return;
    }
    store.sink.print(text);
}

static uint64_t appliance_boot_log_store_ticks(void)
{
    return store.sink.ticks != NULL ? store.sink.ticks() : 0U;
}

/* A bounded line builder: every store record is assembled here, so no record
 * can exceed the bound the console and the log slot share. */
struct appliance_boot_log_store_line {
    char text[APPLIANCE_BOOT_LOG_STORE_LINE_MAX];
    size_t length;
};

static void appliance_boot_log_store_line_begin(
    struct appliance_boot_log_store_line *line,
    const char *prefix)
{
    line->length = 0U;
    line->text[0] = '\0';
    while (prefix != NULL && prefix[line->length] != '\0' &&
           line->length + 1U < sizeof(line->text)) {
        line->text[line->length] = prefix[line->length];
        ++line->length;
    }
    line->text[line->length] = '\0';
}

static void appliance_boot_log_store_line_text(
    struct appliance_boot_log_store_line *line,
    const char *text)
{
    size_t index = 0U;

    if (text == NULL) {
        return;
    }
    while (text[index] != '\0' &&
           line->length + 1U < sizeof(line->text)) {
        line->text[line->length++] = text[index++];
    }
    line->text[line->length] = '\0';
}

static void appliance_boot_log_store_line_u64(
    struct appliance_boot_log_store_line *line,
    uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    if (value == 0U) {
        appliance_boot_log_store_line_text(line, "0");
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
        appliance_boot_log_store_line_text(line, single);
    }
}

static void appliance_boot_log_store_line_publish(
    struct appliance_boot_log_store_line *line)
{
    if (line->length == 0U) {
        return;
    }
    appliance_boot_log_store_publish(line->text);
}

/*
 * ITEM-581: the interactive reading paints its own lines.
 *
 * The dump used the record publication, which is the serial stream and the
 * durable journal only - so `bootlog` answered with nothing on the screen the
 * operator was looking at, while the serial capture showed the records.  The
 * display sink is the screen writer (and, in the kernel's binding, the serial
 * stream beside it); it never reaches the console journal, so the reading
 * cannot be staged back into the log it is reading.
 */
static void appliance_boot_log_store_line_display(
    struct appliance_boot_log_store_line *line)
{
    if (line->length == 0U) {
        return;
    }
    appliance_boot_log_store_print(line->text);
}

/* ITEM-602: the same bounded line, handed to a reader without printing it. */
static void appliance_boot_log_store_line_read(
    struct appliance_boot_log_store_line *line,
    appliance_boot_log_store_reader_fn reader,
    void *context)
{
    if (line->length == 0U || reader == NULL) {
        return;
    }
    reader(context, line->text, line->length);
}

/*
 * `[BOOTLOG]` is the one record shape this module owns.
 *
 *   [BOOTLOG] state=READY path=\EFI\HFOC\BOOTLOG.BIN esp_first_lba=2048
 *       esp_sectors=67584 file_bytes=262144 slot=512 slots=512 records=511
 *       reused=1 gen=0 next=12 written=12 dropped=0 flushed=4 reason=readiness
 *   [BOOTLOG] state=UNAVAILABLE reason=<status> [esp_first_lba=...]
 *
 * A reader of the serial stream - or of the framebuffer, or of the log itself
 * on the next boot - can tell from the first field alone whether the log it is
 * looking at is complete.
 */
static void appliance_boot_log_store_publish_state(
    const char *state,
    const char *reason,
    uint64_t flushed)
{
    struct appliance_boot_log_store_line line;

    appliance_boot_log_store_line_begin(&line, "[BOOTLOG] state=");
    appliance_boot_log_store_line_text(&line, state);
    if (strcmp(state, "READY") == 0) {
        appliance_boot_log_store_line_text(
            &line, " path=" APPLIANCE_BOOT_LOG_PATH);
        appliance_boot_log_store_line_text(&line, " esp_first_lba=");
        appliance_boot_log_store_line_u64(&line, store.esp_first_lba);
        appliance_boot_log_store_line_text(&line, " esp_sectors=");
        appliance_boot_log_store_line_u64(&line, store.esp_sectors);
        appliance_boot_log_store_line_text(&line, " file_bytes=");
        appliance_boot_log_store_line_u64(
            &line, APPLIANCE_BOOT_LOG_FILE_BYTES);
        appliance_boot_log_store_line_text(&line, " slot=");
        appliance_boot_log_store_line_u64(
            &line, APPLIANCE_BOOT_LOG_SLOT_SIZE);
        appliance_boot_log_store_line_text(&line, " slots=");
        appliance_boot_log_store_line_u64(
            &line, APPLIANCE_BOOT_LOG_SLOT_COUNT);
        appliance_boot_log_store_line_text(&line, " records=");
        appliance_boot_log_store_line_u64(
            &line, APPLIANCE_BOOT_LOG_RECORD_CAPACITY);
        appliance_boot_log_store_line_text(&line, " reused=");
        appliance_boot_log_store_line_text(
            &line, store.reused ? "1" : "0");
    } else if (store.bound) {
        appliance_boot_log_store_line_text(&line, " esp_first_lba=");
        appliance_boot_log_store_line_u64(&line, store.esp_first_lba);
    }
    if (reason != NULL) {
        appliance_boot_log_store_line_text(&line, " reason=");
        appliance_boot_log_store_line_text(&line, reason);
    }
    appliance_boot_log_store_line_text(&line, " gen=");
    appliance_boot_log_store_line_u64(&line, store.header.generation);
    appliance_boot_log_store_line_text(&line, " next=");
    appliance_boot_log_store_line_u64(&line, store.ring.next_sequence);
    appliance_boot_log_store_line_text(&line, " written=");
    appliance_boot_log_store_line_u64(&line, store.ring.written_sequence);
    appliance_boot_log_store_line_text(&line, " dropped=");
    appliance_boot_log_store_line_u64(&line, store.ring.dropped);
    appliance_boot_log_store_line_text(&line, " cursor=");
    appliance_boot_log_store_line_u64(&line, store.header.cursor);
    if (flushed != 0U) {
        appliance_boot_log_store_line_text(&line, " flushed=");
        appliance_boot_log_store_line_u64(&line, flushed);
    }
    appliance_boot_log_store_line_publish(&line);
}

/* The one-time transition to UNAVAILABLE.  Publishing it through the console
 * makes it a record like any other - it reaches the screen, the serial stream
 * and (if the log ever becomes writable) the log itself. */
static void appliance_boot_log_store_publish_unavailable(
    enum appliance_boot_log_store_status status,
    const char *reason)
{
    if (store.unavailable_published) {
        return;
    }
    store.unavailable_published = true;
    store.status = status;
    store.failure_reason = reason;
    appliance_boot_log_store_publish_state(
        "UNAVAILABLE",
        reason != NULL ? reason
                       : appliance_boot_log_store_status_string(status),
        0U);
}

/* ------------------------------------------------------- the ESP layer ---- */

static enum efi_boot_variable_io_status
appliance_boot_log_store_variable_read(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    /* The boot log owns no boot variable, so it has none to read. */
    (void)context;
    (void)key;
    (void)data;
    (void)capacity;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_NOT_FOUND;
}

static enum efi_boot_variable_io_status
appliance_boot_log_store_variable_write(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    /* The installer owns the boot entry; the log writes no firmware state. */
    (void)context;
    (void)key;
    (void)data;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_ACCESS_DENIED;
}

static enum appliance_boot_log_store_status
appliance_boot_log_store_map(enum efi_fat32_esp_status status)
{
    switch (status) {
    case EFI_FAT32_ESP_OK:
        return APPLIANCE_BOOT_LOG_STORE_OK;
    case EFI_FAT32_ESP_CAPACITY:
    case EFI_FAT32_ESP_SET_TOO_LARGE:
        return APPLIANCE_BOOT_LOG_STORE_NO_SPACE;
    case EFI_FAT32_ESP_IO_ERROR:
    case EFI_FAT32_ESP_FLUSH_ERROR:
        return APPLIANCE_BOOT_LOG_STORE_IO_ERROR;
    case EFI_FAT32_ESP_NULL:
    case EFI_FAT32_ESP_BAD_STATE:
    case EFI_FAT32_ESP_BAD_ENVIRONMENT:
    case EFI_FAT32_ESP_BAD_DEVICE:
    case EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY:
    case EFI_FAT32_ESP_OUT_OF_RANGE:
    case EFI_FAT32_ESP_CORRUPT:
    case EFI_FAT32_ESP_PATH_DENIED:
    case EFI_FAT32_ESP_NOT_FOUND:
    case EFI_FAT32_ESP_PATH_CONFLICT:
        return APPLIANCE_BOOT_LOG_STORE_BAD_VOLUME;
    default:
        return APPLIANCE_BOOT_LOG_STORE_UNREADABLE;
    }
}

/*
 * ITEM-487: render one record into its slot, without writing it.
 *
 * The flush used to render and write in one step, and every step cost a path
 * lookup and a walk of the file's whole FAT chain.  Rendering is what the flush
 * needs per record; writing is what it now pays once per run of consecutive
 * slots.
 */
static bool appliance_boot_log_store_render_slot(
    const struct appliance_boot_log_write_plan *plan,
    const struct appliance_boot_log_pending_entry *pending,
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE])
{
    struct appliance_boot_log_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.sequence = pending->sequence;
    entry.generation = plan->generation;
    entry.ticks = pending->ticks;
    entry.length = pending->length;
    memcpy(entry.text, pending->text, pending->length);
    return appliance_boot_log_render_record(&entry, slot) ==
           APPLIANCE_BOOT_LOG_OK;
}

static enum appliance_boot_log_store_status
appliance_boot_log_store_write_header(void)
{
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE];
    enum efi_fat32_esp_status status;

    if (appliance_boot_log_render_header(&store.header, slot) !=
        APPLIANCE_BOOT_LOG_OK) {
        return APPLIANCE_BOOT_LOG_STORE_UNREADABLE;
    }
    status = efi_fat32_esp_write_file_range(
        &store.esp, APPLIANCE_BOOT_LOG_PATH, 0U, slot, sizeof(slot));
    if (status != EFI_FAT32_ESP_OK) {
        return appliance_boot_log_store_map(status);
    }
    status = efi_fat32_esp_flush(&store.esp);
    if (status != EFI_FAT32_ESP_OK) {
        return APPLIANCE_BOOT_LOG_STORE_IO_ERROR;
    }
    store.header_written = true;
    return APPLIANCE_BOOT_LOG_STORE_OK;
}

/* Read a byte range of the log file, or report why it cannot be read. */
static enum appliance_boot_log_store_status appliance_boot_log_store_read_range(
    uint32_t offset,
    unsigned char *buffer,
    size_t length)
{
    enum efi_fat32_esp_status status = efi_fat32_esp_read_file_range(
        &store.esp, APPLIANCE_BOOT_LOG_PATH, offset, buffer, length,
        NULL, NULL);

    return appliance_boot_log_store_map(status);
}

/*
 * The header slot was torn or written by a version we do not know: rebuild what
 * the log's own records prove.  Every slot carries its sequence and generation,
 * so the newest valid record is the end of the log whatever the header says -
 * which is exactly the rule `scripts/hfoc-esp-read.py --bootlog` applies, and
 * the reason a torn header cannot make the log unreadable.
 */
static void appliance_boot_log_store_scavenge(void)
{
    unsigned char chunk[APPLIANCE_BOOT_LOG_STORE_SCAVENGE_CHUNK];
    struct appliance_boot_log_entry entry;
    uint32_t slot;
    bool found = false;
    uint32_t newest_slot = 0U;
    uint64_t newest_sequence = 0U;
    uint64_t newest_generation = 0U;

    for (slot = 1U; slot < APPLIANCE_BOOT_LOG_SLOT_COUNT;
         slot += (uint32_t)(sizeof(chunk) / APPLIANCE_BOOT_LOG_SLOT_SIZE)) {
        const uint32_t offset = slot * APPLIANCE_BOOT_LOG_SLOT_SIZE;
        const size_t slots_in_chunk =
            sizeof(chunk) / APPLIANCE_BOOT_LOG_SLOT_SIZE;
        size_t chunk_bytes = sizeof(chunk);
        size_t index;

        if (chunk_bytes > APPLIANCE_BOOT_LOG_FILE_BYTES - offset) {
            chunk_bytes = APPLIANCE_BOOT_LOG_FILE_BYTES - offset;
        }
        if (chunk_bytes < APPLIANCE_BOOT_LOG_SLOT_SIZE ||
            appliance_boot_log_store_read_range(offset, chunk, chunk_bytes) !=
                APPLIANCE_BOOT_LOG_STORE_OK) {
            return;
        }
        for (index = 0U; index < slots_in_chunk; ++index) {
            const uint32_t candidate =
                slot + (uint32_t)index;

            if (candidate >= APPLIANCE_BOOT_LOG_SLOT_COUNT) {
                break;
            }
            if (appliance_boot_log_parse_slot(
                    chunk + index * APPLIANCE_BOOT_LOG_SLOT_SIZE,
                    &entry) != APPLIANCE_BOOT_LOG_SLOT_OK) {
                continue;
            }
            if (!found || entry.sequence >= newest_sequence) {
                found = true;
                newest_sequence = entry.sequence;
                newest_generation = entry.generation;
                newest_slot = candidate;
            }
        }
    }
    if (!found) {
        return;
    }
    /*
     * Resume after the newest record the extent really holds.  The write
     * counter is not recoverable from the records alone, so it reports what the
     * log can prove: the sequence it has reached.
     */
    store.header.cursor =
        (uint64_t)(newest_slot % APPLIANCE_BOOT_LOG_RECORD_CAPACITY);
    /* The record after the last slot of a pass opens the next generation. */
    store.header.generation =
        store.header.cursor == 0U ? newest_generation + 1U
                                  : newest_generation;
    store.header.next_sequence = newest_sequence + 1U;
    store.header.written = newest_sequence + 1U;
    store.header.dropped = 0U;
}

static enum appliance_boot_log_store_status appliance_boot_log_store_open(
    struct block_device *disk,
    uint64_t first_lba,
    uint64_t block_count,
    bool manifest_required)
{
    enum efi_boot_environment_status environment_status;
    enum efi_fat32_esp_status fat_status;
    enum appliance_boot_log_store_status status;
    unsigned char header_slot[APPLIANCE_BOOT_LOG_SLOT_SIZE];
    struct efi_fat32_esp *esp = &store.esp;
    bool present = false;
    uint32_t manifest_size = 0U;
    uint32_t first_cluster = 0U;
    bool reused = false;

    if (disk == NULL) {
        return APPLIANCE_BOOT_LOG_STORE_NULL;
    }
    memset(&store.environment, 0, sizeof(store.environment));
    environment_status = efi_boot_environment_init(
        &store.environment, EFI_BOOT_FIRMWARE_UEFI, disk, NULL,
        appliance_boot_log_store_variable_read,
        appliance_boot_log_store_variable_write);
    memset(esp, 0, sizeof(*esp));
    if (environment_status == EFI_BOOT_ENVIRONMENT_OK) {
        fat_status = efi_fat32_esp_bind(
            &store.environment, first_lba, block_count, esp);
    } else {
        /*
         * ITEM-517: a legacy-BIOS boot has no EFI environment to construct,
         * and the volume it booted from is a plain FAT boot volume, not an
         * ESP.  The environment stays the first choice - a UEFI boot binds
         * exactly as it always did - and a boot without one binds the same
         * volume by device and extent through the env-free path the BIOS boot
         * chain itself uses.  A bind that fails here is mapped exactly like
         * the environment path's failure: no path of this module reports a
         * success it did not have.
         */
        fat_status = efi_fat32_esp_bind_device(
            disk, first_lba, block_count, esp);
    }
    if (fat_status != EFI_FAT32_ESP_OK) {
        return appliance_boot_log_store_map(fat_status);
    }
    if (manifest_required) {
        /*
         * Is this ESP ours?  The installed boot manifest is what the install
         * staged and what the boot chain verified on this very boot, so an ESP
         * that carries it is the one this appliance belongs to.  An ESP without
         * it is somebody else's and is left exactly as it is.
         */
        fat_status = efi_fat32_esp_probe_file(
            esp, APPLIANCE_BOOT_LOG_STORE_MANIFEST_PATH, &present,
            &manifest_size);
        if (fat_status != EFI_FAT32_ESP_OK) {
            return appliance_boot_log_store_map(fat_status);
        }
        if (!present || manifest_size == 0U) {
            return APPLIANCE_BOOT_LOG_STORE_NOT_OURS;
        }
    }
    fat_status = efi_fat32_esp_create_fixed_file(
        esp, APPLIANCE_BOOT_LOG_PATH,
        (uint32_t)APPLIANCE_BOOT_LOG_FILE_BYTES, &first_cluster, &reused);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return appliance_boot_log_store_map(fat_status);
    }
    (void)first_cluster;

    memset(&store.header, 0, sizeof(store.header));
    store.header.version = APPLIANCE_BOOT_LOG_VERSION;
    store.header.slot_size = APPLIANCE_BOOT_LOG_SLOT_SIZE;
    store.header.slot_count = APPLIANCE_BOOT_LOG_SLOT_COUNT;
    store.header.record_capacity = APPLIANCE_BOOT_LOG_RECORD_CAPACITY;
    if (reused) {
        status = appliance_boot_log_store_read_range(
            0U, header_slot, sizeof(header_slot));
        if (status != APPLIANCE_BOOT_LOG_STORE_OK) {
            return status;
        }
        if (appliance_boot_log_parse_header(header_slot, &store.header) ==
            APPLIANCE_BOOT_LOG_OK) {
            /* The header is the log's own claim about its cursor. */
            if (store.header.next_sequence <
                store.header.written) {
                store.header.written = store.header.next_sequence;
            }
        } else {
            /* A torn or unknown header: rebuild from the records themselves. */
            memset(&store.header, 0, sizeof(store.header));
            store.header.version = APPLIANCE_BOOT_LOG_VERSION;
            store.header.slot_size = APPLIANCE_BOOT_LOG_SLOT_SIZE;
            store.header.slot_count = APPLIANCE_BOOT_LOG_SLOT_COUNT;
            store.header.record_capacity =
                APPLIANCE_BOOT_LOG_RECORD_CAPACITY;
            appliance_boot_log_store_scavenge();
            store.header_written = false;
        }
    }
    if (!reused) {
        store.header_written = false;
    }
    store.reused = reused;
    store.esp_first_lba = first_lba;
    store.esp_sectors = block_count;
    /*
     * ITEM-497: the verdicts the boots before this one left in the tail are
     * read back before anything this boot publishes, so the first digest of
     * this boot already carries them and the boot numbers itself after them.
     */
    appliance_boot_log_store_load_verdicts();
    /*
     * The ring resumes where the extent stopped.  The records this boot
     * published before the log was attached - the install records, the boot
     * chain, the storage verdict - are *not* discarded: they are renumbered
     * onto the extent's sequence, so an install and the boot that followed it
     * are one sequence in one file.
     */
    store.ring.dropped += store.header.dropped;
    if (appliance_boot_log_ring_rebase(
            &store.ring, store.header.next_sequence) !=
        APPLIANCE_BOOT_LOG_OK) {
        /* Nothing to carry over (or the pending range is not contiguous):
         * resume from what the extent itself holds. */
        store.ring.next_sequence = store.header.next_sequence;
        store.ring.written_sequence = store.header.next_sequence;
    }
    store.bound = true;
    store.status = APPLIANCE_BOOT_LOG_STORE_OK;
    /*
     * A newly created extent - and one whose header was torn and had to be
     * rebuilt from its records - is made self-describing here rather than at
     * the first flush: a machine that dies before anything else happens should
     * still leave a file a reader recognises as an HFOC boot log.  A failure
     * is not fatal and is retried by the next flush.
     */
    if (!store.header_written) {
        (void)appliance_boot_log_store_write_header();
    }
    return APPLIANCE_BOOT_LOG_STORE_OK;
}

enum appliance_boot_log_store_status appliance_boot_log_store_attach_disk(
    struct block_device *disk,
    const char *reason)
{
    struct appliance_gpt_hfoc_partition partition;
    uint32_t index;

    if (disk == NULL) {
        return APPLIANCE_BOOT_LOG_STORE_NULL;
    }
    if (store.bound) {
        return APPLIANCE_BOOT_LOG_STORE_OK;
    }
    for (index = 0U;
         index < APPLIANCE_BOOT_LOG_STORE_ESP_CANDIDATES; ++index) {
        enum appliance_boot_log_store_status status;
        enum appliance_gpt_discovery_status gpt_status;

        memset(&partition, 0, sizeof(partition));
        gpt_status = appliance_gpt_find_esp_partition(
            disk, index, &partition);
        if (gpt_status == APPLIANCE_GPT_DISCOVERY_NOT_FOUND) {
            break;
        }
        if (gpt_status != APPLIANCE_GPT_DISCOVERY_OK) {
            /*
             * A disk that carries no partition table this module can read is
             * simply not a log target: a data disk on the same machine is
             * probed and left alone, exactly like an ESP that is not ours.
             */
            return APPLIANCE_BOOT_LOG_STORE_NOT_OURS;
        }
        status = appliance_boot_log_store_open(
            disk, partition.first_lba, partition.block_count, true);
        if (status == APPLIANCE_BOOT_LOG_STORE_NOT_OURS) {
            continue;
        }
        if (status != APPLIANCE_BOOT_LOG_STORE_OK) {
            return status;
        }
        appliance_boot_log_store_publish_state("READY", reason, 0U);
        return APPLIANCE_BOOT_LOG_STORE_OK;
    }
    return APPLIANCE_BOOT_LOG_STORE_NOT_OURS;
}

enum appliance_boot_log_store_status appliance_boot_log_store_attach_extent(
    struct block_device *disk,
    uint64_t first_lba,
    uint64_t block_count,
    const char *reason)
{
    enum appliance_boot_log_store_status status;

    if (disk == NULL) {
        return APPLIANCE_BOOT_LOG_STORE_NULL;
    }
    if (store.bound) {
        return APPLIANCE_BOOT_LOG_STORE_OK;
    }
    status = appliance_boot_log_store_open(
        disk, first_lba, block_count, false);
    if (status != APPLIANCE_BOOT_LOG_STORE_OK) {
        return status;
    }
    appliance_boot_log_store_publish_state("READY", reason, 0U);
    return APPLIANCE_BOOT_LOG_STORE_OK;
}

/* ------------------------------------------------------ the flush path ---- */

enum appliance_boot_log_store_status appliance_boot_log_store_init(
    const struct appliance_boot_log_store_sink *sink)
{
    if (sink == NULL || sink->publish == NULL) {
        return APPLIANCE_BOOT_LOG_STORE_NULL;
    }
    memset(&store, 0, sizeof(store));
    store.sink = *sink;
    store.next_boot = 1U;
    appliance_boot_log_gate_init(&store.gate);
    if (appliance_boot_log_ring_init(&store.ring) !=
        APPLIANCE_BOOT_LOG_OK) {
        return APPLIANCE_BOOT_LOG_STORE_BAD_STATE;
    }
    store.initialized = true;
    store.status = APPLIANCE_BOOT_LOG_STORE_NO_TARGET;
    return APPLIANCE_BOOT_LOG_STORE_OK;
}

void appliance_boot_log_store_flush(const char *reason)
{
    struct appliance_boot_log_pending_entry pending;
    struct appliance_boot_log_write_plan plan;
    enum appliance_boot_log_store_status status;
    uint64_t flushed = 0U;
    bool failed = false;

    if (!store.initialized || store.in_flush) {
        return;
    }
    if (!store.ring.active) {
        return;
    }
    if (!store.bound) {
        if (appliance_boot_log_ring_state(&store.ring, NULL, NULL) !=
            APPLIANCE_BOOT_LOG_OK) {
            return;
        }
        appliance_boot_log_store_publish_unavailable(
            APPLIANCE_BOOT_LOG_STORE_NO_TARGET,
            appliance_boot_log_store_status_string(
                APPLIANCE_BOOT_LOG_STORE_NO_TARGET));
        return;
    }
    store.in_flush = true;
    {
        const uint64_t flush_started = timer_cycles();
        uint64_t write_us = 0U;
        uint64_t commit_us = 0U;
        uint64_t text_bytes = 0U;
        uint64_t batch_count = 0U;

        /* ITEM-487: the reading describes this flush's own writes. */
        efi_fat32_esp_io_stats_reset();
        /*
         * ITEM-487: write the pending records as runs of consecutive slots.
         *
         * Every file-range call resolves the path and walks the file's FAT
         * chain from its first cluster to the offset, so the old one-call-per
         * record loop paid both for every 512-byte slot: `hw15` measured ~42 ms
         * per record and 2.7-3.2 s for 64 of them, all of it blocked main loop.
         * A run of consecutive slots is one range write - the writer already
         * handles a range that crosses clusters, a range that ends at the
         * extent's last byte and a range that is not sector aligned, and it
         * refuses a range that would grow the file - and the records are
         * retired only after the bytes are on the device, so the ring's
         * `written_sequence` stays the honest count of what the extent holds
         * and a failed write leaves the whole run pending.
         *
         * The run ends where the file's own ring wraps (the next slot is not
         * the next index), and it is bounded by the pending ring, so the buffer
         * is a fixed 32 KiB and one flush is at most two range writes.
         */
        while (!failed) {
            uint32_t first_slot = 0U;
            uint64_t batch_records = 0U;
            uint64_t batch_text_bytes = 0U;
            size_t batch_bytes = 0U;
            enum appliance_boot_log_status ring_status;

            while (batch_records < APPLIANCE_BOOT_LOG_PENDING_CAPACITY) {
                ring_status = appliance_boot_log_ring_peek(
                    &store.ring, &store.header, batch_records, &pending,
                    &plan);
                if (ring_status == APPLIANCE_BOOT_LOG_EMPTY) {
                    break;
                }
                if (ring_status != APPLIANCE_BOOT_LOG_OK) {
                    failed = true;
                    break;
                }
                if (batch_records == 0U) {
                    first_slot = plan.slot_index;
                } else if (plan.slot_index !=
                           first_slot + (uint32_t)batch_records) {
                    /* The file's ring wrapped: the run ends here. */
                    break;
                }
                if (!appliance_boot_log_store_render_slot(
                        &plan, &pending,
                        store_flush_batch + batch_bytes)) {
                    failed = true;
                    break;
                }
                batch_bytes += APPLIANCE_BOOT_LOG_SLOT_SIZE;
                batch_text_bytes += (uint64_t)pending.length;
                ++batch_records;
            }
            if (failed || batch_records == 0U) {
                break;
            }
            {
                const uint64_t write_started = timer_cycles();
                const enum efi_fat32_esp_status esp_status =
                    efi_fat32_esp_write_file_range(
                        &store.esp, APPLIANCE_BOOT_LOG_PATH,
                        (uint32_t)(first_slot *
                                   APPLIANCE_BOOT_LOG_SLOT_SIZE),
                        store_flush_batch, batch_bytes);

                write_us += timer_cycles_to_us(timer_cycles() - write_started);
                batch_count += 1U;
                status = appliance_boot_log_store_map(esp_status);
            }
            if (status != APPLIANCE_BOOT_LOG_STORE_OK) {
                failed = true;
                break;
            }
            {
                uint64_t retired;

                for (retired = 0U; retired < batch_records; ++retired) {
                    if (appliance_boot_log_ring_next(
                            &store.ring, &store.header, &pending, &plan) !=
                            APPLIANCE_BOOT_LOG_OK ||
                        appliance_boot_log_advance_cursor(
                            store.header.cursor, &store.header.cursor,
                            &store.header.generation) !=
                            APPLIANCE_BOOT_LOG_OK ||
                        appliance_boot_log_ring_mark_written(&store.ring) !=
                            APPLIANCE_BOOT_LOG_OK) {
                        failed = true;
                        break;
                    }
                }
            }
            if (failed) {
                break;
            }
            text_bytes += batch_text_bytes;
            flushed += batch_records;
            store.header.next_sequence = store.ring.next_sequence;
            store.header.written = store.ring.written_sequence;
            store.header.dropped = store.ring.dropped;
        }
        if (!failed && (flushed != 0U || !store.header_written)) {
            const uint64_t commit_started = timer_cycles();

            status = appliance_boot_log_store_write_header();
            commit_us += timer_cycles_to_us(timer_cycles() - commit_started);
            if (status != APPLIANCE_BOOT_LOG_STORE_OK) {
                failed = true;
            }
        }
        {
            struct efi_fat32_esp_io_stats esp_stats;
            const uint64_t total_us = timer_cycles_to_us(
                timer_cycles() - flush_started);
            const uint64_t measured = write_us + commit_us;

            memset(&esp_stats, 0, sizeof(esp_stats));
            (void)efi_fat32_esp_io_stats_read(&esp_stats);
            store_flush_stats.records = (uint32_t)(flushed > UINT32_MAX
                                                       ? UINT32_MAX
                                                       : flushed);
            store_flush_stats.text_bytes = text_bytes;
            store_flush_stats.slot_bytes =
                flushed * (uint64_t)APPLIANCE_BOOT_LOG_SLOT_SIZE;
            store_flush_stats.esp_write_us = write_us;
            store_flush_stats.commit_us = commit_us;
            store_flush_stats.total_us = total_us;
            store_flush_stats.publish_us =
                total_us > measured ? total_us - measured : 0U;
            store_flush_stats.lookup_us = esp_stats.lookup_us;
            store_flush_stats.fat_traversal_us = esp_stats.traversal_us;
            store_flush_stats.batch_count = batch_count;
            store_flush_stats.count += 1U;
        }
    }
    /*
     * The completion record is published while the flush is still marked as
     * running: it names `[BOOTLOG]`, which is itself a flush trigger, and a
     * publication that re-entered the flush would recurse for as long as the
     * records kept differing.  The record it publishes is staged like any
     * other and reaches the extent at the next flush.
     */
    if (failed) {
        /*
         * Best effort, as documented: the records stay staged - the ring only
         * retires a record whose slot reached the device - and the failure is
         * published once so an operator sees the log is not durable.
         */
        appliance_boot_log_store_publish_unavailable(
            APPLIANCE_BOOT_LOG_STORE_IO_ERROR,
            appliance_boot_log_store_status_string(
                APPLIANCE_BOOT_LOG_STORE_IO_ERROR));
    } else {
        store.unavailable_published = false;
        store.status = APPLIANCE_BOOT_LOG_STORE_OK;
        store.failure_reason = NULL;
        if (flushed != 0U) {
            appliance_boot_log_store_publish_state(
                "READY", reason, flushed);
        }
    }
    store.in_flush = false;
}

void appliance_boot_log_store_panic_flush(void)
{
    if (!store.initialized) {
        return;
    }
    store.panic = true;
    /*
     * The panic path is the one place where the last records matter most and
     * where nothing may be attempted twice: one bounded flush, and every
     * failure is swallowed by the flush itself.
     */
    appliance_boot_log_store_flush("panic");
}

void appliance_boot_log_store_observe(const char *record, size_t length)
{
    if (!store.initialized || record == NULL || length == 0U) {
        return;
    }
    /*
     * ITEM-518: the dump reads the log through the publish funnel, so a dump
     * that is observed would feed its own output back into the ring it is
     * reading.  A reading is not a record.
     */
    if (store.dumping) {
        return;
    }
    /*
     * The console repaints nothing when a poll loop republishes an unchanged
     * state; the log keeps the same rule for the same reason.  Repetition of an
     * *unchanged* state carries no information, and keeping it out of the ring
     * is what lets the bounded ring hold the history that does.
     */
    if (store.dedup_valid && store.dedup_length == length &&
        memcmp(store.dedup, record, length) == 0) {
        return;
    }
    store.dedup_valid = true;
    store.dedup_length = length <= APPLIANCE_BOOT_LOG_TEXT_MAX
                             ? length
                             : APPLIANCE_BOOT_LOG_TEXT_MAX;
    memcpy(store.dedup, record, store.dedup_length);
    store.dedup[store.dedup_length] = '\0';
    /*
     * ITEM-497: a periodic record family is admitted once and then at most
     * once per interval, so the ring keeps a history of the boot instead of
     * the last few seconds of it.  The gate never holds a state change back:
     * the families it names are the per-second snapshots, and everything a
     * refusal, a watchdog episode or a flush carries is not one of them.
     */
    if (!appliance_boot_log_store_gate_admit(
            record, length, appliance_boot_log_store_ticks())) {
        return;
    }
    if (length > APPLIANCE_BOOT_LOG_TEXT_MAX) {
        /*
         * The console bounds a record well below the slot, so this is a
         * defensive path for a caller that publishes its own record: it is
         * truncated and marked, never dropped, so the log stays a record of
         * what happened rather than of what fitted.
         */
        static char truncated[APPLIANCE_BOOT_LOG_TEXT_MAX + 1U];

        memcpy(truncated, record, APPLIANCE_BOOT_LOG_TEXT_MAX);
        memcpy(truncated + APPLIANCE_BOOT_LOG_TEXT_MAX - 3U, "...", 3U);
        truncated[APPLIANCE_BOOT_LOG_TEXT_MAX] = '\0';
        /*
         * ITEM-521 (F-A): the second consumer, the install journal on the
         * medium, receives exactly the records this policy admitted - and it
         * receives them with the text the ring stages.  Handing them over
         * after the dedup and the gate is what keeps one admission policy for
         * both logs; handing them over before the ring append is deliberate,
         * because the journal's own bounded ring names its own losses and must
         * not lose a record the store's ring is too full to take.
         */
        if (store.sink.admitted != NULL) {
            store.sink.admitted(truncated, APPLIANCE_BOOT_LOG_TEXT_MAX);
        }
        (void)appliance_boot_log_ring_append(
            &store.ring, truncated, APPLIANCE_BOOT_LOG_TEXT_MAX,
            appliance_boot_log_store_ticks(), NULL);
    } else {
        if (store.sink.admitted != NULL) {
            store.sink.admitted(record, length);
        }
        if (appliance_boot_log_ring_append(
                &store.ring, record, length,
                appliance_boot_log_store_ticks(), NULL) !=
            APPLIANCE_BOOT_LOG_OK) {
            return;
        }
    }
    {
        const char *reason = "trigger";

        if (memcmp(record, "[PRODUCTION-DHCP]", 17U) == 0) {
            reason = "dhcp";
        } else if (memcmp(record, "[PRODUCTION-DNS]", 16U) == 0) {
            reason = "dns";
        } else if (memcmp(record, "[PRODUCTION-SERVING]", 20U) == 0) {
            reason = "serving";
        } else if (memcmp(record, "[APPLIANCE-BOOT]", 16U) == 0) {
            reason = "readiness";
        } else if (memcmp(record, "[PRODUCTION-INSTALL]", 19U) == 0) {
            reason = "install";
        } else if (memcmp(record, "[BOOT-CHAIN]", 12U) == 0) {
            reason = "boot-chain";
        } else if (memcmp(record, "[BOOTLOG]", 9U) == 0) {
            reason = "state";
        }
        /*
         * ITEM-497: the loop's own heartbeat is the one periodic record that
         * already reaches this funnel, so it is where the input verdict set is
         * restated: the records are staged before the heartbeat's flush and
         * leave with it, and a boot that wraps the ring still leaves every
         * recent boot's verdict findable in the tail.
         */
        if (memcmp(record, "[PRODUCTION-LOOP]", 17U) == 0) {
            appliance_boot_log_store_input_digest();
        }
        if (appliance_boot_log_flush_trigger(record, length)) {
            appliance_boot_log_store_flush(reason);
        }
    }
}

/* --------------------------------------------------------- ITEM-497 ------ */

static bool appliance_boot_log_store_gate_admit(
    const char *record,
    size_t length,
    uint64_t ticks)
{
    size_t family;

    if (record == NULL || length == 0U) {
        return true;
    }
    for (family = 0U; family < APPLIANCE_BOOT_LOG_STORE_GATE_FAMILIES;
         ++family) {
        const char *const marker =
            appliance_boot_log_store_gate_markers[family];
        const size_t marker_length = strlen(marker);

        if (length >= marker_length &&
            memcmp(record, marker, marker_length) == 0) {
            /*
             * A record that differs from the last one this family published is
             * a state change: it takes a slot now.  Only an unchanged repeat
             * waits for the family's interval, and even then the interval
             * restates it so a reader can tell "nothing changed" from
             * "the machine stopped".
             */
            if (family < APPLIANCE_BOOT_LOG_STORE_GATE_BY_TIME) {
                const bool same =
                    store.gate_last_valid[family] &&
                    store.gate_last_length[family] == length &&
                    memcmp(store.gate_last[family], record, length) == 0;

                if (!same) {
                    store.gate_last_valid[family] = true;
                    store.gate_last_length[family] =
                        length <= APPLIANCE_BOOT_LOG_TEXT_MAX
                            ? length
                            : APPLIANCE_BOOT_LOG_TEXT_MAX;
                    memcpy(store.gate_last[family], record,
                           store.gate_last_length[family]);
                    store.gate_last[family][store.gate_last_length[family]] =
                        '\0';
                    return true;
                }
            }
            return appliance_boot_log_gate_admit(
                &store.gate, family,
                appliance_boot_log_store_gate_intervals[family], ticks);
        }
    }
    return true;
}

/*
 * Stage a record without the dedup cache, the gate or the flush policy: the
 * verdict digest is a deliberate repetition, and it must leave with the
 * heartbeat's own flush rather than trigger a write of its own for every copy.
 */
static void appliance_boot_log_store_stage(const char *record, size_t length)
{
    if (!store.initialized || record == NULL || length == 0U ||
        length > APPLIANCE_BOOT_LOG_TEXT_MAX) {
        return;
    }
    (void)appliance_boot_log_ring_append(
        &store.ring, record, length, appliance_boot_log_store_ticks(), NULL);
}

/*
 * Restate the verdict set: this boot's own verdict, then the two boots before
 * it.  The carried bodies are the earlier boots' own text, marked
 * `[INPUT-PATH-CARRY]`, so a reader sees each boot's verdict and its boot
 * number rather than a summary that hides which boot failed.
 */
static void appliance_boot_log_store_input_digest(void)
{
    char record[APPLIANCE_BOOT_LOG_STORE_RECORD_MAX];
    const uint64_t now = appliance_boot_log_store_ticks();
    size_t index;

    if (!store.initialized || !store.bound || !store.input_valid) {
        return;
    }
    if (store.digest_ticks != 0U &&
        now - store.digest_ticks <
            APPLIANCE_BOOT_LOG_STORE_VERDICT_DIGEST_TICKS) {
        return;
    }
    store.digest_ticks = now;
    for (index = 0U; index <= store.carried_count; ++index) {
        const char *const marker =
            index == 0U ? "[INPUT-PATH] " : "[INPUT-PATH-CARRY] ";
        const char *const body =
            index == 0U ? store.input : store.carried[index - 1U].text;
        const size_t marker_length = strlen(marker);
        const size_t body_length = strlen(body);
        size_t offset = 0U;

        if (marker_length + body_length >= sizeof(record)) {
            continue;
        }
        memcpy(record, marker, marker_length);
        offset = marker_length;
        memcpy(record + offset, body, body_length);
        offset += body_length;
        appliance_boot_log_store_stage(record, offset);
    }
}

/* One body per boot, newest first: the two boots a verdict carries. */
static void appliance_boot_log_store_verdict_merge(
    const struct appliance_boot_log_input_record *found,
    size_t found_count)
{
    size_t index;

    for (index = 0U; index < found_count; ++index) {
        size_t position = 0U;

        while (position < store.carried_count &&
               store.carried[position].boot > found[index].boot) {
            ++position;
        }
        if (position < store.carried_count &&
            store.carried[position].boot == found[index].boot) {
            continue;
        }
        if (store.carried_count < APPLIANCE_BOOT_LOG_INPUT_CARRY_MAX) {
            store.carried_count += 1U;
        } else if (position >= store.carried_count) {
            continue;
        }
        for (size_t move = store.carried_count - 1U; move > position; --move) {
            store.carried[move] = store.carried[move - 1U];
        }
        store.carried[position] = found[index];
    }
}

/*
 * Read the tail of the extent the previous boots wrote and keep the verdicts
 * they left there.  The window is walked oldest first in chunks, so the buffer
 * stays bounded and a chunk that cannot be read ends the scan instead of
 * failing the attach: a log that cannot be read back is still a log.
 */
static void appliance_boot_log_store_load_verdicts(void)
{
    unsigned char chunk[APPLIANCE_BOOT_LOG_SLOT_SIZE *
                        APPLIANCE_BOOT_LOG_STORE_VERDICT_CHUNK_SLOTS];
    struct appliance_boot_log_input_record found[
        APPLIANCE_BOOT_LOG_INPUT_CARRY_MAX];
    const uint64_t capacity = APPLIANCE_BOOT_LOG_RECORD_CAPACITY;
    uint64_t newest;
    uint64_t newest_boot = 0U;
    uint64_t count;
    uint64_t start;

    store.carried_count = 0U;
    store.next_boot = 1U;
    if (store.header.cursor == 0U) {
        return;
    }
    newest = (store.header.cursor - 1U) % capacity;
    count = store.header.cursor <
                    APPLIANCE_BOOT_LOG_STORE_VERDICT_SCAN_SLOTS
                ? store.header.cursor
                : APPLIANCE_BOOT_LOG_STORE_VERDICT_SCAN_SLOTS;
    start = (newest + capacity - (count - 1U)) % capacity;
    while (count > 0U) {
        uint64_t run = capacity - start;
        const size_t run_bytes =
            (size_t)(run > count ? count : run) *
            APPLIANCE_BOOT_LOG_SLOT_SIZE;
        size_t chunk_bytes = run_bytes;
        size_t slots;
        size_t found_count;
        uint64_t chunk_newest = 0U;

        if (chunk_bytes > sizeof(chunk)) {
            chunk_bytes = sizeof(chunk);
        }
        slots = chunk_bytes / APPLIANCE_BOOT_LOG_SLOT_SIZE;
        if (slots == 0U ||
            appliance_boot_log_store_read_range(
                (uint32_t)((1U + start) * APPLIANCE_BOOT_LOG_SLOT_SIZE),
                chunk, slots * APPLIANCE_BOOT_LOG_SLOT_SIZE) !=
                APPLIANCE_BOOT_LOG_STORE_OK) {
            return;
        }
        found_count = appliance_boot_log_scan_input_verdicts(
            chunk, slots, found, APPLIANCE_BOOT_LOG_INPUT_CARRY_MAX,
            &chunk_newest);
        if (chunk_newest > newest_boot) {
            newest_boot = chunk_newest;
        }
        appliance_boot_log_store_verdict_merge(found, found_count);
        count -= slots;
        start = (start + slots) % capacity;
    }
    /* The boot after the newest verdict one numbers itself - and a log with no
     * verdict at all (an upgrade, or a ring that lost them) starts at one
     * instead of claiming a boot that never wrote one. */
    store.next_boot = newest_boot + 1U;
}

void appliance_boot_log_store_input_state(
    const char *keyboard,
    const char *mouse,
    uint64_t arm_ms,
    uint64_t first_report_ms,
    uint64_t ports,
    uint64_t reports)
{
    struct appliance_boot_log_input_state state;
    char body[APPLIANCE_BOOT_LOG_INPUT_BODY_MAX + 1U];
    char record[APPLIANCE_BOOT_LOG_STORE_RECORD_MAX];
    size_t body_length;
    size_t record_length;

    if (!store.initialized || !store.bound) {
        return;
    }
    memset(&state, 0, sizeof(state));
    state.boot = store.next_boot != 0U ? store.next_boot : 1U;
    state.keyboard = keyboard;
    state.mouse = mouse;
    state.arm_ms = arm_ms;
    state.first_report_ms = first_report_ms;
    state.ports = ports;
    state.reports = reports;
    body_length =
        appliance_boot_log_format_input_body(body, sizeof(body), &state);
    if (body_length == 0U) {
        return;
    }
    /*
     * One record per boot, or per state change: a verdict that repeats is the
     * digest's business, and the ring is scarce.
     */
    if (store.input_valid && store.input_length == body_length &&
        memcmp(store.input, body, body_length) == 0) {
        return;
    }
    store.input_valid = true;
    store.input_length = body_length;
    memcpy(store.input, body, body_length + 1U);
    record_length = 0U;
    {
        static const char marker[] = "[INPUT-PATH] ";

        memcpy(record, marker, sizeof(marker) - 1U);
        record_length = sizeof(marker) - 1U;
    }
    if (record_length + body_length >= sizeof(record)) {
        return;
    }
    memcpy(record + record_length, body, body_length);
    record_length += body_length;
    /* A flush trigger: the verdict of the boot that just failed is on the disk
     * before the next record is published. */
    appliance_boot_log_store_observe(record, record_length);
    store.digest_ticks = appliance_boot_log_store_ticks();
}

void appliance_boot_log_store_input_verdict_tick(void)
{
    appliance_boot_log_store_input_digest();
}

void appliance_boot_log_store_dump(void)
{
    struct appliance_boot_log_pending_entry entry;
    struct appliance_boot_log_store_line line;
    uint64_t sequence;
    uint64_t first;
    uint64_t end;
    uint64_t shown = 0U;

    if (!store.initialized) {
        appliance_boot_log_store_print("BOOTLOG: UNAVAILABLE\n");
        return;
    }
    /*
     * ITEM-518: one dump is one bounded, finite reading.
     *
     * `line_publish` reaches the console journal, the journal hands the record
     * back to `observe`, and the old loop compared its counter with
     * `store.ring.next_sequence` - a value the dump's own output moved.  On the
     * owner's laptop that made `bootlog` never return, and a command that
     * cannot return cannot diagnose anything.  The end is captured once, the
     * dump is not observed while it runs, and only the staging window is read,
     * so the work is bounded before the first line is painted.  Progress lines
     * every `APPLIANCE_BOOT_LOG_DUMP_CHUNK` records show a screen that is still
     * printing rather than a screen that has stopped.
     */
    end = store.ring.next_sequence;
    store.dumping = true;
    appliance_boot_log_store_line_begin(&line, "BOOTLOG: state=");
    appliance_boot_log_store_line_text(
        &line, store.bound ? "READY" : "UNAVAILABLE");
    if (!store.bound) {
        appliance_boot_log_store_line_text(&line, " reason=");
        appliance_boot_log_store_line_text(
            &line,
            store.failure_reason != NULL
                ? store.failure_reason
                : appliance_boot_log_store_status_string(
                      APPLIANCE_BOOT_LOG_STORE_NO_TARGET));
    } else {
        appliance_boot_log_store_line_text(
            &line, " path=" APPLIANCE_BOOT_LOG_PATH " esp_first_lba=");
        appliance_boot_log_store_line_u64(&line, store.esp_first_lba);
    }
    appliance_boot_log_store_line_text(&line, " records=");
    appliance_boot_log_store_line_u64(&line, store.ring.next_sequence);
    appliance_boot_log_store_line_text(&line, " pending=");
    appliance_boot_log_store_line_u64(
        &line, store.ring.next_sequence - store.ring.written_sequence);
    appliance_boot_log_store_line_text(&line, " dropped=");
    appliance_boot_log_store_line_u64(&line, store.ring.dropped);
    appliance_boot_log_store_line_text(&line, " window=");
    appliance_boot_log_store_line_u64(
        &line, (uint64_t)APPLIANCE_BOOT_LOG_PENDING_CAPACITY);
    appliance_boot_log_store_line_text(&line, "\n");
    appliance_boot_log_store_line_display(&line);

    first = end > APPLIANCE_BOOT_LOG_PENDING_CAPACITY
                ? end - APPLIANCE_BOOT_LOG_PENDING_CAPACITY
                : 0U;
    for (sequence = first; sequence < end; ++sequence) {
        if (appliance_boot_log_ring_entry(&store.ring, sequence, &entry) !=
            APPLIANCE_BOOT_LOG_OK) {
            continue;
        }
        appliance_boot_log_store_line_begin(&line, "#");
        appliance_boot_log_store_line_u64(&line, entry.sequence);
        appliance_boot_log_store_line_text(&line, " ");
        appliance_boot_log_store_line_text(&line, entry.text);
        appliance_boot_log_store_line_text(&line, "\n");
        appliance_boot_log_store_line_display(&line);
        ++shown;
        if (shown % APPLIANCE_BOOT_LOG_DUMP_CHUNK == 0U) {
            appliance_boot_log_store_line_begin(&line, "BOOTLOG: progress=");
            appliance_boot_log_store_line_u64(&line, shown);
            appliance_boot_log_store_line_text(&line, " of=");
            appliance_boot_log_store_line_u64(&line, end - first);
            appliance_boot_log_store_line_text(&line, "\n");
            appliance_boot_log_store_line_display(&line);
        }
    }
    if (shown == 0U) {
        store.dumping = false;
        appliance_boot_log_store_print(
            "BOOTLOG: EMPTY records=0 window=0\n");
        return;
    }
    /*
     * The reading says how much of the window it showed and that it stopped at
     * the captured end: a screen that ends here is a finished command, not a
     * hung one.
     */
    appliance_boot_log_store_line_begin(&line, "BOOTLOG: shown=");
    appliance_boot_log_store_line_u64(&line, shown);
    appliance_boot_log_store_line_text(&line, " of=");
    appliance_boot_log_store_line_u64(&line, end - first);
    appliance_boot_log_store_line_text(&line, " done\n");
    appliance_boot_log_store_line_display(&line);
    /*
     * The footer belongs to the reading too: `dumping` is cleared only after
     * the last line it publishes, or the footer would be staged as a record of
     * the boot and a second `bootlog` would report one more record than the
     * first.
     */
    store.dumping = false;
}

uint32_t appliance_boot_log_store_read_records(
    appliance_boot_log_store_reader_fn reader,
    void *context,
    uint32_t max_records)
{
    struct appliance_boot_log_pending_entry entry;
    struct appliance_boot_log_store_line line;
    uint64_t sequence;
    uint64_t first;
    uint64_t end;
    uint32_t shown = 0U;

    if (reader == NULL) {
        return 0U;
    }
    if (!store.initialized) {
        static const char unavailable[] = "BOOTLOG: UNAVAILABLE\n";

        reader(context, unavailable, sizeof(unavailable) - 1U);
        return 0U;
    }
    end = store.ring.next_sequence;
    appliance_boot_log_store_line_begin(&line, "BOOTLOG: state=");
    appliance_boot_log_store_line_text(
        &line, store.bound ? "READY" : "UNAVAILABLE");
    if (!store.bound) {
        appliance_boot_log_store_line_text(&line, " reason=");
        appliance_boot_log_store_line_text(
            &line,
            store.failure_reason != NULL
                ? store.failure_reason
                : appliance_boot_log_store_status_string(
                      APPLIANCE_BOOT_LOG_STORE_NO_TARGET));
    } else {
        appliance_boot_log_store_line_text(
            &line, " path=" APPLIANCE_BOOT_LOG_PATH " esp_first_lba=");
        appliance_boot_log_store_line_u64(&line, store.esp_first_lba);
    }
    appliance_boot_log_store_line_text(&line, " records=");
    appliance_boot_log_store_line_u64(&line, store.ring.next_sequence);
    appliance_boot_log_store_line_text(&line, " pending=");
    appliance_boot_log_store_line_u64(
        &line, store.ring.next_sequence - store.ring.written_sequence);
    appliance_boot_log_store_line_text(&line, " dropped=");
    appliance_boot_log_store_line_u64(&line, store.ring.dropped);
    appliance_boot_log_store_line_text(&line, " window=");
    appliance_boot_log_store_line_u64(
        &line, (uint64_t)APPLIANCE_BOOT_LOG_PENDING_CAPACITY);
    appliance_boot_log_store_line_text(&line, "\n");
    appliance_boot_log_store_line_read(&line, reader, context);

    first = end > APPLIANCE_BOOT_LOG_PENDING_CAPACITY
                ? end - APPLIANCE_BOOT_LOG_PENDING_CAPACITY
                : 0U;
    if (max_records == 0U) {
        first = end;
    } else if (end - first > (uint64_t)max_records) {
        first = end - (uint64_t)max_records;
    }
    for (sequence = first; sequence < end; ++sequence) {
        if (appliance_boot_log_ring_entry(&store.ring, sequence, &entry) !=
            APPLIANCE_BOOT_LOG_OK) {
            continue;
        }
        appliance_boot_log_store_line_begin(&line, "#");
        appliance_boot_log_store_line_u64(&line, entry.sequence);
        appliance_boot_log_store_line_text(&line, " ");
        appliance_boot_log_store_line_text(&line, entry.text);
        appliance_boot_log_store_line_text(&line, "\n");
        appliance_boot_log_store_line_read(&line, reader, context);
        ++shown;
    }
    return shown;
}

bool appliance_boot_log_store_ready(void)
{
    return store.bound;
}

enum appliance_boot_log_store_status appliance_boot_log_store_status(void)
{
    return store.status;
}

bool appliance_boot_log_store_extent(
    uint64_t *first_lba_out,
    uint64_t *block_count_out)
{
    if (!store.bound) {
        return false;
    }
    if (first_lba_out != NULL) {
        *first_lba_out = store.esp_first_lba;
    }
    if (block_count_out != NULL) {
        *block_count_out = store.esp_sectors;
    }
    return true;
}

const char *appliance_boot_log_store_status_string(
    enum appliance_boot_log_store_status status)
{
    switch (status) {
    case APPLIANCE_BOOT_LOG_STORE_OK:
        return "APPLIANCE_BOOT_LOG_STORE_OK";
    case APPLIANCE_BOOT_LOG_STORE_NULL:
        return "APPLIANCE_BOOT_LOG_STORE_NULL";
    case APPLIANCE_BOOT_LOG_STORE_NO_TARGET:
        return "NO_TARGET";
    case APPLIANCE_BOOT_LOG_STORE_NOT_OURS:
        return "NO_ESP";
    case APPLIANCE_BOOT_LOG_STORE_BAD_ENVIRONMENT:
        return "BAD_ENVIRONMENT";
    case APPLIANCE_BOOT_LOG_STORE_BAD_VOLUME:
        return "BAD_VOLUME";
    case APPLIANCE_BOOT_LOG_STORE_NO_SPACE:
        return "NO_SPACE";
    case APPLIANCE_BOOT_LOG_STORE_IO_ERROR:
        return "IO_ERROR";
    case APPLIANCE_BOOT_LOG_STORE_UNREADABLE:
        return "UNREADABLE";
    case APPLIANCE_BOOT_LOG_STORE_BAD_STATE:
        return "BAD_STATE";
    default:
        return NULL;
    }
}

/*
 * ITEM-487: the flush reading, as one bounded journal record.
 *
 * The reading has more fields than one 320-byte line can carry in the worst
 * case - nine decimal fields of twenty digits cannot fit beside the literals -
 * so it is published as the record plus its `-TAIL` continuation, the same
 * shape the other bounded diagnostics of this kernel use.  Both halves are
 * assembled whole and a half that would not fit is refused (a length of zero)
 * rather than truncated: a reader that sees the record sees every field it
 * names, or sees no record at all.
 */
struct appliance_boot_log_flush_writer {
    char *buffer;
    size_t size;
    size_t length;
    bool overflow;
};

static void appliance_boot_log_flush_write_text(
    struct appliance_boot_log_flush_writer *writer,
    const char *text)
{
    size_t index = 0U;

    if (writer->overflow || text == NULL) {
        return;
    }
    while (text[index] != '\0') {
        if (writer->length + 1U >= writer->size) {
            writer->overflow = true;
            return;
        }
        writer->buffer[writer->length++] = text[index++];
    }
    writer->buffer[writer->length] = '\0';
}

static void appliance_boot_log_flush_write_u64(
    struct appliance_boot_log_flush_writer *writer,
    uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    if (writer->overflow) {
        return;
    }
    if (value == 0U) {
        appliance_boot_log_flush_write_text(writer, "0");
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
        appliance_boot_log_flush_write_text(writer, single);
    }
}

static size_t appliance_boot_log_store_flush_record_finish(
    const struct appliance_boot_log_flush_writer *writer)
{
    if (writer->overflow || writer->length == 0U ||
        writer->length > APPLIANCE_BOOT_LOG_FLUSH_RECORD_BYTES) {
        return 0U;
    }
    return writer->length;
}

size_t appliance_boot_log_store_format_flush_record(
    char *buffer,
    size_t size,
    const struct appliance_boot_log_flush_stats *stats)
{
    struct appliance_boot_log_flush_writer writer;

    if (buffer == NULL || size == 0U || stats == NULL) {
        return 0U;
    }
    writer.buffer = buffer;
    writer.size = size;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';
    appliance_boot_log_flush_write_text(&writer, "[BOOTLOG-FLUSH] records=");
    appliance_boot_log_flush_write_u64(&writer, stats->records);
    appliance_boot_log_flush_write_text(&writer, " text_bytes=");
    appliance_boot_log_flush_write_u64(&writer, stats->text_bytes);
    appliance_boot_log_flush_write_text(&writer, " slot_bytes=");
    appliance_boot_log_flush_write_u64(&writer, stats->slot_bytes);
    appliance_boot_log_flush_write_text(&writer, " esp_write_us=");
    appliance_boot_log_flush_write_u64(&writer, stats->esp_write_us);
    appliance_boot_log_flush_write_text(&writer, " commit_us=");
    appliance_boot_log_flush_write_u64(&writer, stats->commit_us);
    appliance_boot_log_flush_write_text(&writer, " total_us=");
    appliance_boot_log_flush_write_u64(&writer, stats->total_us);
    return appliance_boot_log_store_flush_record_finish(&writer);
}

size_t appliance_boot_log_store_format_flush_tail_record(
    char *buffer,
    size_t size,
    const struct appliance_boot_log_flush_stats *stats)
{
    struct appliance_boot_log_flush_writer writer;

    if (buffer == NULL || size == 0U || stats == NULL) {
        return 0U;
    }
    writer.buffer = buffer;
    writer.size = size;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';
    appliance_boot_log_flush_write_text(&writer,
                                        "[BOOTLOG-FLUSH-TAIL] publish_us=");
    appliance_boot_log_flush_write_u64(&writer, stats->publish_us);
    appliance_boot_log_flush_write_text(&writer, " publish=derived lookup_us=");
    appliance_boot_log_flush_write_u64(&writer, stats->lookup_us);
    appliance_boot_log_flush_write_text(&writer, " fat_traversal_us=");
    appliance_boot_log_flush_write_u64(&writer, stats->fat_traversal_us);
    appliance_boot_log_flush_write_text(&writer, " batch_count=");
    appliance_boot_log_flush_write_u64(&writer, stats->batch_count);
    appliance_boot_log_flush_write_text(
        &writer, " unavailable=esp_blocks,clusters");
    return appliance_boot_log_store_flush_record_finish(&writer);
}

bool appliance_boot_log_store_last_flush(
    struct appliance_boot_log_flush_stats *out)
{
    if (out == NULL || store_flush_stats.count == 0U) {
        return false;
    }
    *out = store_flush_stats;
    return true;
}
