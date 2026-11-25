#ifndef HFOC_APPLIANCE_BOOT_LOG_STORE_H
#define HFOC_APPLIANCE_BOOT_LOG_STORE_H

#include "appliance_boot_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "efi_fat32_esp.h"

/*
 * ITEM-410: the durable half of the appliance boot log.
 *
 * The records this kernel publishes are appended to the bounded in-RAM staging
 * ring by an observer the local console calls for every record it accepts, and
 * written to `\EFI\HFOC\BOOTLOG.BIN` on the EFI System Partition at the moments
 * that matter.  The file is created once, preallocated to
 * APPLIANCE_BOOT_LOG_FILE_BYTES, and every later boot appends to the extent it
 * finds instead of starting over, so a hardware run reads the install records
 * and the boots that followed them in one log.
 *
 * Three rules shape this module:
 *
 *   * the log is best effort.  A machine whose ESP cannot be written publishes
 *     `[BOOTLOG] state=UNAVAILABLE reason=...` and boots on; no path in this
 *     module can fail, delay past one bounded flush or refuse a boot;
 *   * the log writes only what is ours.  It attaches to an ESP that carries
 *     this appliance's own installed boot manifest, it creates exactly one file
 *     in the directory our install owns, and after that first creation every
 *     flush overwrites slots inside that file's own clusters - the file
 *     allocation table, the directory and every other file on a shared ESP are
 *     never touched again;
 *   * the log never writes a disk that is not the declared target.  On the
 *     installer medium the module is not attached at boot at all: the install
 *     flow attaches it to the disk it just installed to, after the boot stage.
 */

/* How many EFI System Partitions one disk is probed for, in GPT slot order. */
#define APPLIANCE_BOOT_LOG_STORE_ESP_CANDIDATES 4U
/*
 * The marker that says an ESP is an HFOC install's own: the sealed boot
 * manifest the installed boot set stages and the boot chain verifies.  An ESP
 * without it belongs to somebody else and is never written - a Windows ESP on
 * the same machine is probed and left alone.
 */
#define APPLIANCE_BOOT_LOG_STORE_MANIFEST_PATH "\\boot\\hfoc-boot.man"

enum appliance_boot_log_store_status {
    APPLIANCE_BOOT_LOG_STORE_OK = 0,
    APPLIANCE_BOOT_LOG_STORE_NULL = 1,
    APPLIANCE_BOOT_LOG_STORE_NO_TARGET = 2,
    APPLIANCE_BOOT_LOG_STORE_NOT_OURS = 3,
    APPLIANCE_BOOT_LOG_STORE_BAD_ENVIRONMENT = 4,
    APPLIANCE_BOOT_LOG_STORE_BAD_VOLUME = 5,
    APPLIANCE_BOOT_LOG_STORE_NO_SPACE = 6,
    APPLIANCE_BOOT_LOG_STORE_IO_ERROR = 7,
    APPLIANCE_BOOT_LOG_STORE_UNREADABLE = 8,
    APPLIANCE_BOOT_LOG_STORE_BAD_STATE = 9,
};

/*
 * What the store needs from its owner: the console's record publication, a
 * text writer for the local `BOOTLOG` dump, and the tick counter a record is
 * stamped with.  Injected rather than called directly so the store can be
 * proven on the host against a file-backed device.
 */
struct appliance_boot_log_store_sink {
    void (*publish)(const char *record, size_t length);
    void (*print)(const char *text);
    uint64_t (*ticks)(void);
    /*
     * ITEM-521: every record the store admits - past the dump guard, the
     * repeated-record dedup and the periodic-family gate - is handed to this
     * second consumer with the same text the ring stages.  The install journal
     * on the medium must hold the set the durable log admitted, not the
     * funnel's repaints (F-A of item-521-wiring-audit-2026-10-12.md): a
     * bounded journal fed from the funnel itself fills with heartbeats and
     * loses the announcement and the observed storage that requirement 1
     * names.  Optional: NULL means no second consumer.
     */
    void (*admitted)(const char *record, size_t length);
};

enum appliance_boot_log_store_status appliance_boot_log_store_init(
    const struct appliance_boot_log_store_sink *sink);
/*
 * One record, as the console publishes it: staged in RAM, and - when it names
 * one of the moments that matter - written out at once.
 */
void appliance_boot_log_store_observe(const char *record, size_t length);

/*
 * ITEM-487: what the last flush cost, and where the cost went.
 *
 * `hw15` measured a 64-record flush of the durable log at 2.7-3.2 s of blocked
 * main loop, ~42 ms for every 512-byte record, while the keyboard's last
 * completion sat 3.45 s behind the flush that blocked it.  Those numbers are
 * about the flush as a whole and about the range writer's shape, so this is the
 * reading that separates them:
 *
 *   * `records`, `text_bytes`, `slot_bytes` - what reached the extent;
 *   * `publish_us` - the flush's own bookkeeping outside the write and the
 *     commit, named derived because the plan walk and the slot write share one
 *     loop and cannot be separated without changing the flush;
 *   * `esp_write_us` - the records' file-range writes, measured around the
 *     calls that make them;
 *   * `commit_us` - the header slot and the device flush that makes the log
 *     durable;
 *   * `total_us` - the whole flush, from the cycle counter;
 *   * `lookup_us`, `fat_traversal_us` - the two spans inside those writes,
 *     read from the ESP writer's own probe: path resolution, and the FAT chain
 *     walk that reaches the offset.  With one sector per cluster the walk costs
 *     one sector read per slot already written, which is why the number grows
 *     with the offset;
 *   * `batch_count` - how many file-range writes carried the records.  One per
 *     record today; the reading that shows a batch actually batched.
 *
 * The struct is a counter beside the work, not a participant in it: no path
 * reads it, and nothing here changes when or how the journal writes.
 */
struct appliance_boot_log_flush_stats {
    uint32_t records;
    uint64_t text_bytes;
    uint64_t slot_bytes;
    uint64_t publish_us;
    uint64_t esp_write_us;
    uint64_t commit_us;
    uint64_t total_us;
    uint64_t lookup_us;
    uint64_t fat_traversal_us;
    uint64_t batch_count;
    /* Monotonic: a caller publishes one record per new value. */
    uint64_t count;
};

/* The last completed flush, or false when none has run yet. */
bool appliance_boot_log_store_last_flush(
    struct appliance_boot_log_flush_stats *out);

/*
 * The `[BOOTLOG-FLUSH]` line for one reading, or 0 when it does not fit - the
 * record is bounded by APPLIANCE_BOOT_LOG_FLUSH_RECORD_BYTES so the durable
 * journal can carry it whole; a value that would overflow the bound is refused
 * rather than truncated, and the refusal is a length of zero.
 */
#define APPLIANCE_BOOT_LOG_FLUSH_RECORD_BYTES 320U
size_t appliance_boot_log_store_format_flush_record(
    char *buffer,
    size_t size,
    const struct appliance_boot_log_flush_stats *stats);
/*
 * The rest of the reading, as the `[BOOTLOG-FLUSH-TAIL]` continuation: the
 * derived publish span, the two spans inside the writes, the number of range
 * writes that carried the records, and what this layer cannot read at all.
 */
size_t appliance_boot_log_store_format_flush_tail_record(
    char *buffer,
    size_t size,
    const struct appliance_boot_log_flush_stats *stats);

/*
 * Attach to the EFI System Partition of `disk` that carries this appliance's
 * own installed boot set.  Read-only until the ESP proves it is ours: an ESP
 * without the manifest is reported NOT_OURS and no byte of it is written.
 * Idempotent - a second call for the disk already bound does nothing.
 */
enum appliance_boot_log_store_status appliance_boot_log_store_attach_disk(
    struct block_device *disk,
    const char *reason);
/*
 * Attach to an extent the caller already knows is ours - the ESP role the
 * install flow has just staged into.  No manifest probe, because the caller
 * just wrote the manifest there.
 */
enum appliance_boot_log_store_status appliance_boot_log_store_attach_extent(
    struct block_device *disk,
    uint64_t first_lba,
    uint64_t block_count,
    const char *reason);
/*
 * Write every staged record into the extent and flush the device.  Bounded by
 * the ring; a failure publishes `state=UNAVAILABLE` once and leaves the records
 * staged for the next flush.  `reason` names the moment in the published state
 * record (`readiness`, `dhcp`, `serving`, `refusal`, `panic`, ...).
 */
void appliance_boot_log_store_flush(const char *reason);
/*
 * ITEM-497: the input path reports its per-boot verdict here, once per boot or
 * per state change.  The store renders it as `[INPUT-PATH] boot=N verdict=...
 * keyboard=... mouse=... arm_ms=... first_report_ms=... ports=... reports=...`
 * - the boot number is the newest one the extent already carries plus one,
 * read back at attach - and publishes it as a flush marker, so the boot that
 * just lost its input is on the disk it came up on.  An unchanged verdict is
 * not published again; the store's own digest (re-stated with the loop
 * heartbeat) is what keeps it, and the two boots before it, in the ring's
 * tail.  A call before the extent is attached is ignored - the verdict exists
 * to be durable.
 */
void appliance_boot_log_store_input_state(
    const char *keyboard,
    const char *mouse,
    uint64_t arm_ms,
    uint64_t first_report_ms,
    uint64_t ports,
    uint64_t reports);
/*
 * ITEM-497: restate the verdict set if the digest window has passed.  The input
 * path calls this with its own once-a-second publication, so the carried
 * verdicts do not depend on the cadence of an unrelated record: the set is
 * restated while the machine runs, whatever else is being published.
 */
void appliance_boot_log_store_input_verdict_tick(void);
/* The panic path: write what is staged *now* and never panic inside. */
void appliance_boot_log_store_panic_flush(void);
/* The local `BOOTLOG` console command: the staged records, from RAM. */
void appliance_boot_log_store_dump(void);

/*
 * ITEM-602: the same bounded reading without printing it.  Emits the log's own
 * state line and the newest `max_records` staged records, oldest first, each
 * as the `#<sequence> <text>\n` form the console dump uses.  It never observes
 * or republishes the reading, so an SSH `hfoc journal` cannot feed itself back
 * into the log.  Returns the number of body records emitted; 0 is a valid
 * answer for a log that has no staged records (the state line still names why).
 */
typedef void (*appliance_boot_log_store_reader_fn)(void *context,
                                                   const char *text,
                                                   size_t length);
uint32_t appliance_boot_log_store_read_records(
    appliance_boot_log_store_reader_fn reader,
    void *context,
    uint32_t max_records);

bool appliance_boot_log_store_ready(void);
enum appliance_boot_log_store_status appliance_boot_log_store_status(void);
const char *appliance_boot_log_store_status_string(
    enum appliance_boot_log_store_status status);
/* The extent the log lives in, for a record that has to name it. */
bool appliance_boot_log_store_extent(
    uint64_t *first_lba_out,
    uint64_t *block_count_out);

#endif
