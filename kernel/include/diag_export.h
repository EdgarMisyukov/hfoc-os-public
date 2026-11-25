#ifndef HFOC_DIAG_EXPORT_H
#define HFOC_DIAG_EXPORT_H

/*
 * ITEM-552: one operator action, one file on the boot medium, one command on
 * the Debian machine.
 *
 * The diagnostic work before this item ended at the screen: the failure
 * snapshot lives in RAM, the journal is written "when the medium is reachable",
 * and the only way to carry either to the machine that can read them is a
 * photograph.  That is what this module removes.  `diag save` composes one
 * bounded report - the build identity, the controllers, the USB medium and the
 * pointer, the xHCI and input answers and the saved command snapshot - writes
 * it into the journal the medium's build reserved, flushes, reads the slots
 * back and verifies them, and only then says SAVED with a session identifier.
 * `scripts/hfoc-diag-read.py` turns the medium into `HFOC-DIAG-<session>.txt`
 * on the operator's own machine with no package and no network.
 *
 * The report is a session, not a pile of records: a BEGIN record names the
 * identifier and the build, an END record names how many records the session
 * carries and a CRC over their text, and the identifier comes from the counter
 * the journal header persists - so two saves on one medium can never share an
 * identifier, and an extractor can refuse a torn or stale session instead of
 * presenting it as fresh.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag_failure.h"
#include "install_journal_core.h"

#define DIAG_SESSION_DIGITS 8U
/* One report record is one journal slot's text: the journal's own bound. */
#define DIAG_RECORD_TEXT_MAX INSTALL_JOURNAL_TEXT_MAX
/* A continuation record starts with this and carries the rest of a long line. */
#define DIAG_CONTINUATION "[DIAG+] "
#define DIAG_CONTINUATION_BYTES (sizeof(DIAG_CONTINUATION) - 1U)
/*
 * The report is bounded by construction: the sinks below cannot compose more
 * than this many records, and the session refuses to save a report that would
 * not fit rather than truncating a fact out of it.
 */
/* ITEM-582: the report now carries every EHCI configuration candidate
   ([EHCI-CONFIG]) beside the xHCI/EHCI snapshots and the network/panel/entropy
   records; 48 records no longer held a medium with several candidates and the
   export refused with OVERFLOW instead of writing.  The reserved journal is
   512 slots, so the bound is the report, not the medium. */
#define DIAG_REPORT_MAX_RECORDS 96U
#define DIAG_REPORT_LINE_MAX 192U
#define DIAG_REPORT_RESERVE_SLOTS 2U

enum diag_export_result {
    DIAG_EXPORT_OK = 0,
    DIAG_EXPORT_NO_MEDIUM,
    DIAG_EXPORT_NOT_READY,
    DIAG_EXPORT_FULL,
    DIAG_EXPORT_REWIND,
    DIAG_EXPORT_WRITE,
    DIAG_EXPORT_VERIFY,
    DIAG_EXPORT_OVERFLOW,
};

const char *diag_export_result_string(enum diag_export_result result);

/* What one successful save produced, for the console line. */
struct diag_export_session {
    uint32_t id;
    uint32_t records;
    uint32_t bytes;
};

/*
 * The operator's one action.  It never fails the machine and never claims a
 * save it did not verify: the result is OK only after the slots were read back
 * and their texts, their count, the session identifier and the CRC matched.
 */
enum diag_export_result diag_export_save(struct diag_export_session *session);
/* The reason the last save failed, or "OK" - a short code for the console. */
const char *diag_export_last_reason(void);

/*
 * ITEM-582: the snapshot that exists even when no medium does.
 *
 * The installed machine refused `diag save` with `NO_MEDIUM/MEDIUM_PROBE` and
 * the refusal ended before the network and panel evidence was composed - the
 * one file that could have explained the machine was exactly the file the
 * machine could not write.  The composition now runs before the medium is
 * judged: a save without a medium keeps every record in RAM, the console
 * prints them under the refusal, and `diag snapshot` prints them again.
 * `count` is 0 when no snapshot exists this boot; every index below it is a
 * line already split on record boundaries.
 */
uint32_t diag_export_snapshot_records(void);
const char *diag_export_snapshot_line(uint32_t index);

/*
 * ITEM-602: compose the same bounded snapshot without touching a medium, for
 * the SSH `hfoc diag` stream.  Returns the record count, or 0 when the
 * composition overflowed its own bound - a stream that cannot carry every
 * record must refuse, not publish a truncated report as complete.  The lines
 * are the same `diag_export_snapshot_line()` sequence `diag save` verifies and
 * the Debian reader prints; the caller applies the BEGIN/END/CRC framing.
 */
uint32_t diag_export_snapshot_compose(void);

/*
 * ITEM-557: why the last save failed, in the two sizes the operator needs.
 * ITEM-558 moved the record itself into its own pure module (diag_failure.h) so
 * a host proof can drive it, and added the primary cause: the hardware run
 * after ITEM-557 named the walk's stage (`ehci=DESCRIPTOR_FAILED/DEVICE8`) and
 * lost the transfer that stopped it, which is the one fact that decides the
 * next fix.  `reason=`, `step=` and (when known) `primary=` form the short line
 * that fits one row; the detail carries the machine's answers and the
 * controller's own failure snapshot, and the record outlives the cleanup.
 */
const struct diag_failure *diag_export_last_failure(void);

/*
 * The sink the report's sources write through: the xHCI and EHCI diagnostics
 * answer with one text line per call.  A line longer than one record is split
 * here on a record boundary with DIAG_CONTINUATION in front of every piece, so
 * a long answer is carried whole instead of being clamped away.
 */
typedef void (*diag_report_sink_fn)(void *context, const char *text);

/* ---------------------------------------------------------------- pure half --- */

/* The two framing records.  Both are bounded: a caller that cannot fit the
   whole line gets zero and writes no half-record. */
size_t diag_export_begin_line(char *buffer,
                              size_t size,
                              uint32_t id,
                              const char *build,
                              const char *version,
                              uint32_t written,
                              uint32_t capacity);
size_t diag_export_end_line(char *buffer,
                            size_t size,
                            uint32_t id,
                            uint32_t records,
                            uint32_t crc);
/* What one record means to the report: `id` for BEGIN/END, or none. */
bool diag_export_parse_marker(const char *text,
                              bool *is_begin,
                              bool *is_end,
                              uint32_t *id,
                              uint32_t *records,
                              uint32_t *crc);

/* The session CRC: every record's text followed by one newline, in order. */
uint32_t diag_export_crc(uint32_t crc, const char *text, size_t length);

/* One journal slot as the writer framed it (length, CRC over the text, text).
   Returns the text length, or zero for a torn or foreign slot - the same rule
   the Debian reader applies, so the two sides cannot disagree about what a
   written record is. */
size_t diag_export_slot_text(const uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES],
                             char *buffer,
                             size_t size);

#endif
