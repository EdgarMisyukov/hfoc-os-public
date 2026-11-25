#ifndef HFOC_APPLIANCE_BOOT_LOG_H
#define HFOC_APPLIANCE_BOOT_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-410: the boot log the appliance writes to its own disk.
 *
 * Every record this kernel publishes about an install, a refusal, the network
 * path or its own readiness already reaches the serial stream and (since
 * ITEM-405) the framebuffer console, and `appliance_runtime_log` keeps a
 * bounded structured mirror of them in RAM.  A hardware run, however, ends
 * with photographs of a 16-line screen: one record can scroll away and cost a
 * whole iteration, and a machine whose run refuses before the console is up
 * shows nothing at all.  The appliance owns a disk; the log has to survive the
 * boot on it.
 *
 * What is stored is a *file of ours* on the EFI System Partition -
 * `\EFI\HFOC\BOOTLOG.BIN` - so the result is readable from any Linux with a
 * plain mount and, independently, out of a raw image by
 * `scripts/hfoc-esp-read.py --bootlog`.  The alternative (a dedicated region
 * inside the appliance's own persistence partition) was rejected deliberately:
 * that volume is the state volume, it can be sealed by ITEM-373 and it is not a
 * filesystem a host can mount, so an operator would need HFOC software and a
 * key to read a log that exists to explain a machine that did not come up.
 *
 * This header is the part of the boot log that is pure: the on-disk format, the
 * checksums, the bounded in-RAM staging ring and the flush policy.  It is
 * freestanding and host-testable; nothing here touches a device.  The ESP
 * writer that performs the flush lives in `appliance_boot_log_store.h`.
 *
 * Format (version 1, documented in docs/hfoc-installed-boot.md):
 *
 *   * the file is exactly APPLIANCE_BOOT_LOG_FILE_BYTES bytes, preallocated
 *     once, and is never grown or shrunk afterwards - a flush only ever
 *     overwrites slots inside the extent the file already owns;
 *   * the extent is a flat array of APPLIANCE_BOOT_LOG_SLOT_COUNT slots of
 *     APPLIANCE_BOOT_LOG_SLOT_SIZE bytes;
 *   * slot 0 is the header, slots 1..511 are the records, oldest repeated by
 *     wrapping: record N goes to slot `1 + (cursor % RECORD_CAPACITY)`, the
 *     cursor advances per record and the generation increments when it wraps;
 *   * every slot is one ASCII line closed by '\n' and padded with NUL bytes,
 *     so `cat` of the file on a mounted ESP shows the sequence numbers and the
 *     text without any HFOC tooling;
 *   * every slot carries a CRC-32 over its own meaningful bytes, so a torn
 *     write is detected instead of being printed as text;
 *   * each record carries its own sequence number and its generation, so a
 *     reader can tell the newest contiguous run from the leftovers of the
 *     previous wrap without trusting the header at all.
 */

#define APPLIANCE_BOOT_LOG_VERSION 1U
#define APPLIANCE_BOOT_LOG_SLOT_SIZE 512U
#define APPLIANCE_BOOT_LOG_SLOT_COUNT 512U
#define APPLIANCE_BOOT_LOG_RECORD_CAPACITY \
    (APPLIANCE_BOOT_LOG_SLOT_COUNT - 1U)
#define APPLIANCE_BOOT_LOG_FILE_BYTES \
    (APPLIANCE_BOOT_LOG_SLOT_SIZE * APPLIANCE_BOOT_LOG_SLOT_COUNT)
#define APPLIANCE_BOOT_LOG_MAGIC "HFOC-BOOTLOG"
#define APPLIANCE_BOOT_LOG_PATH "\\EFI\\HFOC\\BOOTLOG.BIN"
/*
 * The text a record slot can carry: the fixed-width field header of a record
 * line plus the closing newline are subtracted from the slot.  The console
 * mirror bounds a record at CONSOLE_RECORD_MAX (320) bytes, so a record this
 * kernel publishes is never truncated by the boot log.
 */
#define APPLIANCE_BOOT_LOG_TEXT_MAX 440U
/*
 * The bounded in-RAM staging ring.  Records are appended as they are published
 * - which is long before any storage exists - and written out at the next
 * flush.  A ring that overflows loses its oldest *unwritten* record and counts
 * it, so the loss is published rather than hidden.
 */
#define APPLIANCE_BOOT_LOG_PENDING_CAPACITY 64U

enum appliance_boot_log_status {
    APPLIANCE_BOOT_LOG_OK = 0,
    APPLIANCE_BOOT_LOG_NULL = 1,
    APPLIANCE_BOOT_LOG_BAD_STATE = 2,
    APPLIANCE_BOOT_LOG_BAD_SLOT = 3,
    APPLIANCE_BOOT_LOG_BAD_VERSION = 4,
    APPLIANCE_BOOT_LOG_BAD_TEXT = 5,
    APPLIANCE_BOOT_LOG_BAD_RANGE = 6,
    APPLIANCE_BOOT_LOG_EMPTY = 7,
    APPLIANCE_BOOT_LOG_TRUNCATED = 8,
};

/* What one slot of the extent turned out to be. */
enum appliance_boot_log_slot_status {
    APPLIANCE_BOOT_LOG_SLOT_EMPTY = 0,
    APPLIANCE_BOOT_LOG_SLOT_OK = 1,
    APPLIANCE_BOOT_LOG_SLOT_TORN = 2,
};

struct appliance_boot_log_header {
    uint64_t version;
    uint64_t slot_size;
    uint64_t slot_count;
    uint64_t record_capacity;
    uint64_t generation;
    uint64_t next_sequence;
    uint64_t cursor;
    uint64_t dropped;
    uint64_t written;
};

struct appliance_boot_log_entry {
    uint64_t sequence;
    uint64_t generation;
    uint64_t ticks;
    size_t length;
    char text[APPLIANCE_BOOT_LOG_TEXT_MAX + 1U];
};

struct appliance_boot_log_pending_entry {
    uint64_t sequence;
    uint64_t ticks;
    size_t length;
    char text[APPLIANCE_BOOT_LOG_TEXT_MAX + 1U];
};

/*
 * The in-RAM ring: what has been published, what has been written to the
 * extent, and what the last flush cost.  `sequence` in each entry is the
 * sequence the record was published with, and the slot it occupies is
 * `sequence % APPLIANCE_BOOT_LOG_PENDING_CAPACITY`, so a flush walks the
 * unwritten sequences and skips the ones the ring had to drop.
 */
struct appliance_boot_log_ring {
    bool active;
    uint64_t next_sequence;
    uint64_t written_sequence;
    uint64_t dropped;
    struct appliance_boot_log_pending_entry entries[
        APPLIANCE_BOOT_LOG_PENDING_CAPACITY];
};

/* The slice of the extent one flush writes. */
struct appliance_boot_log_write_plan {
    uint64_t sequence;
    uint64_t generation;
    uint32_t slot_index;
};

/*
 * CRC-32, IEEE 802.3 (reflected, polynomial 0xEDB88320, initial value
 * 0xFFFFFFFF, final complement) - the checksum `zlib.crc32()` computes, so the
 * host reader and the kernel agree without sharing a library.
 */
uint32_t appliance_boot_log_crc32(const unsigned char *bytes, size_t length);

enum appliance_boot_log_status appliance_boot_log_render_header(
    const struct appliance_boot_log_header *header,
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE]);
enum appliance_boot_log_status appliance_boot_log_parse_header(
    const unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE],
    struct appliance_boot_log_header *header_out);
/*
 * A slot that is entirely zero was never written; a slot that carries bytes
 * and does not parse (magic, field widths, CRC, a text byte outside printable
 * ASCII, a length that disagrees with the line, or trailing bytes that are not
 * NUL) is torn, and a torn slot is never handed back as text.
 */
enum appliance_boot_log_slot_status appliance_boot_log_parse_slot(
    const unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE],
    struct appliance_boot_log_entry *entry_out);
enum appliance_boot_log_status appliance_boot_log_render_record(
    const struct appliance_boot_log_entry *entry,
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE]);

enum appliance_boot_log_status appliance_boot_log_ring_init(
    struct appliance_boot_log_ring *ring);
enum appliance_boot_log_status appliance_boot_log_ring_append(
    struct appliance_boot_log_ring *ring,
    const char *text,
    size_t length,
    uint64_t ticks,
    uint64_t *sequence_out);
/*
 * The next pending record the flush has to write, oldest first.  Returns
 * APPLIANCE_BOOT_LOG_EMPTY when nothing is pending.  A record the ring dropped
 * before it was written is skipped and counted in `ring->dropped`.
 */
enum appliance_boot_log_status appliance_boot_log_ring_next(
    struct appliance_boot_log_ring *ring,
    const struct appliance_boot_log_header *header,
    struct appliance_boot_log_pending_entry *entry_out,
    struct appliance_boot_log_write_plan *plan_out);
/*
 * ITEM-487: the record `index` places after the one `ring_next()` would hand
 * back, read *without* retiring anything.
 *
 * The durable log's flush wrote one slot per file-range call, and the range
 * writer resolves the path and walks the file's whole FAT chain on every call -
 * which is what made a 64-record flush 2.7-3.2 s of blocked main loop.  A flush
 * that wants to hand one range several consecutive slots has to see them before
 * it writes them and retire them only after the bytes are on the device, and
 * this is the reading that lets it: the ring is a value, the answer is a copy,
 * and `written_sequence` does not move.
 *
 * The plan is exactly the one `ring_next()` would build for that record - the
 * slot index and generation the header's cursor will have after the records
 * before it are retired - so the bytes a caller renders from a peek are the
 * bytes it would have written one at a time.  Records the ring dropped before
 * they were ever written are skipped, exactly as `ring_next()` skips them.
 */
enum appliance_boot_log_status appliance_boot_log_ring_peek(
    const struct appliance_boot_log_ring *ring,
    const struct appliance_boot_log_header *header,
    uint64_t index,
    struct appliance_boot_log_pending_entry *entry_out,
    struct appliance_boot_log_write_plan *plan_out);
/*
 * The record `ring_next()` handed back is on the device: retire it.  A caller
 * that wrote nothing must not call this, which is what keeps the ring's
 * `written_sequence` the honest count of what the extent actually holds.
 */
enum appliance_boot_log_status appliance_boot_log_ring_mark_written(
    struct appliance_boot_log_ring *ring);
/*
 * Renumber the records the ring still owes the extent so the oldest of them
 * becomes `first_sequence`.  A record's sequence is assigned when it is
 * published - long before any storage exists - and an extent that already
 * carries history expects the next record at its own `next` number, so the
 * records staged before the attach have to be moved onto the extent's
 * sequence instead of being discarded or written out of order.
 */
enum appliance_boot_log_status appliance_boot_log_ring_rebase(
    struct appliance_boot_log_ring *ring,
    uint64_t first_sequence);
/* The resident record with this sequence, or APPLIANCE_BOOT_LOG_EMPTY when the
 * ring has already overwritten it.  The console dump walks the ring with it. */
enum appliance_boot_log_status appliance_boot_log_ring_entry(
    const struct appliance_boot_log_ring *ring,
    uint64_t sequence,
    struct appliance_boot_log_pending_entry *entry_out);
/*
 * Where a record written at `cursor` lands, and what the cursor and the
 * generation become afterwards.  Pure arithmetic: a reader and a writer must
 * derive the same address from the same cursor.
 */
uint32_t appliance_boot_log_slot_for_cursor(uint64_t cursor);
enum appliance_boot_log_status appliance_boot_log_advance_cursor(
    uint64_t cursor,
    uint64_t *cursor_out,
    uint64_t *generation_out);
/*
 * The moments that matter, as a bounded, documented test over one record:
 * a record that announces a refusal or a failure is written immediately, so a
 * machine that dies right after it still has it on disk, and so is a record
 * that names one of the stages a hardware run quotes (the boot chain, the
 * appliance readiness line, DHCP and DNS, the serving path, the install).
 * Everything else waits for the next flush; the log is never written once per
 * record.
 */
bool appliance_boot_log_flush_trigger(const char *text, size_t length);
/*
 * What the ring still owes the extent: `pending` unwritten records, and the
 * total `dropped` the ring lost before they could be written.  A drop advances
 * `written_sequence` past the lost record, so the pending range is always
 * resident and the drop counter - not a scan - is the honest measure of loss.
 */
enum appliance_boot_log_status appliance_boot_log_ring_state(
    const struct appliance_boot_log_ring *ring,
    uint32_t *pending_out,
    uint64_t *dropped_out);

/*
 * ITEM-497: the input path's own verdict, one record per boot.
 *
 * The 2026-09-30 `hw20` run came up after an install with no keyboard and no
 * pointer, and the next boot of the same machine came up with both.  Nothing
 * could be concluded from the log: the durable ring holds 511 records, one
 * boot publishes thousands, and the boot that failed had been overwritten
 * before the machine was even switched off.  A machine whose input failed on
 * one boot out of two needs a record that answers, per boot, the three
 * questions a reader actually asks:
 *
 *   * was a HID device bound at all - and to which controller and port;
 *   * was its interrupt endpoint armed (`ARMED`), armed but silent
 *     (`NO_REPORTS`), or never armed (`NOT_ARMED`);
 *   * how long after boot did that happen (`arm_ms`, `first_report_ms`).
 *
 * `[INPUT-PATH]` is one line per boot (or per state change), published through
 * the journal like every other record and a flush trigger, so the boot that
 * just failed is on the disk the machine came up on:
 *
 *   [INPUT-PATH] boot=17 verdict=ARMED keyboard=ARMED mouse=ARMED
 *       arm_ms=812 first_report_ms=1340 ports=2 reports=1823
 *
 * The one thing a wrapping ring cannot do by itself is keep that line for
 * three boots: the boot that follows overwrites it.  The record is therefore
 * *carried*: at attach the store scans the tail of its own extent for the
 * verdicts earlier boots wrote, and re-publishes them - the newest two, as
 * `[INPUT-PATH-CARRY]` - whenever it publishes the current one again.  The
 * carried text is the earlier boot's own body, so a reader sees each boot's
 * verdict and its boot number, not a summary of a summary; the marker names
 * the copy so a carried line is never mistaken for a fresh one.
 *
 * Everything here is arithmetic over byte arrays, so the host proof exercises
 * the exact text a hardware run will be read from.
 */
#define APPLIANCE_BOOT_LOG_INPUT_BODY_MAX 224U
/* The earlier boots a verdict re-publication keeps alive. */
#define APPLIANCE_BOOT_LOG_INPUT_CARRY_MAX 2U

struct appliance_boot_log_input_state {
    /* The boot number this verdict belongs to: the extent's newest + 1. */
    uint64_t boot;
    const char *keyboard;
    const char *mouse;
    uint64_t arm_ms;
    uint64_t first_report_ms;
    uint64_t ports;
    uint64_t reports;
};

/* One boot's verdict body, without its marker: `boot=N verdict=...`. */
struct appliance_boot_log_input_record {
    uint64_t boot;
    size_t length;
    char text[APPLIANCE_BOOT_LOG_INPUT_BODY_MAX + 1U];
};

/*
 * The verdict a reader takes from the two role states, worst first: a role
 * that was never bound is `ABSENT`, one bound but not armed `NOT_ARMED`, one
 * armed but silent `NO_REPORTS`, and only both roles armed and reporting is
 * `ARMED`.  `NO_DEVICE` is the machine with no HID role at all.
 */
const char *appliance_boot_log_input_verdict(
    const char *keyboard,
    const char *mouse);
/*
 * The body of one verdict, or 0 when it does not fit - a caller that gets 0
 * publishes nothing rather than a truncated line.
 */
size_t appliance_boot_log_format_input_body(
    char *buffer,
    size_t size,
    const struct appliance_boot_log_input_state *state);
/*
 * Scan a run of rendered slots for the verdicts earlier boots left there.
 * Both markers are read; a body is kept once per boot number, and the newest
 * `capacity` bodies are returned newest first.  `newest_boot_out` is the
 * highest boot number seen anywhere in the run (0 when there is none), which is
 * what the next boot numbers itself after.
 */
size_t appliance_boot_log_scan_input_verdicts(
    const unsigned char *slots,
    size_t slot_count,
    struct appliance_boot_log_input_record *out,
    size_t capacity,
    uint64_t *newest_boot_out);

/*
 * ITEM-497: the bounded repeat gate for the periodic records.
 *
 * A boot publishes several records a second - the input diagnostics, the loop
 * window, the log's own state line - and a ring of 511 slots keeps only the
 * last few seconds of them.  The gate admits the first record of a family and
 * then at most one per interval, so the ring keeps a readable history of the
 * boot instead of one second of it, and counts what it held back so the loss is
 * published rather than hidden.  It is a value with no clock of its own: the
 * caller passes the ticks and the interval, so the host proof drives it
 * directly.
 */
#define APPLIANCE_BOOT_LOG_GATE_FAMILIES 4U

struct appliance_boot_log_gate {
    bool seen[APPLIANCE_BOOT_LOG_GATE_FAMILIES];
    uint64_t last_ticks[APPLIANCE_BOOT_LOG_GATE_FAMILIES];
    uint64_t admitted[APPLIANCE_BOOT_LOG_GATE_FAMILIES];
    uint64_t suppressed[APPLIANCE_BOOT_LOG_GATE_FAMILIES];
};

void appliance_boot_log_gate_init(struct appliance_boot_log_gate *gate);
/* True when this record of `family` may take a slot; false when it is held. */
bool appliance_boot_log_gate_admit(
    struct appliance_boot_log_gate *gate,
    size_t family,
    uint64_t interval_ticks,
    uint64_t ticks);
/* Records the gate held back, over every family. */
uint64_t appliance_boot_log_gate_suppressed(
    const struct appliance_boot_log_gate *gate);

uint32_t appliance_boot_log_crc32_string(const char *text);
const char *appliance_boot_log_status_string(
    enum appliance_boot_log_status status);
const char *appliance_boot_log_slot_status_string(
    enum appliance_boot_log_slot_status status);

#endif
