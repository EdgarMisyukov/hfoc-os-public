/*
 * ITEM-552: the operator's one action - compose, write, read back, verify.
 *
 * What this module is for: the diagnostic work before it could say everything
 * and carry nothing.  The failure snapshot lives in RAM, the journal is written
 * "when the medium is reachable", and the only way to move either to the
 * machine that can read them is a photograph of the screen.  `diag save`
 * turns that into a file: it composes one bounded report from the machine's own
 * answers, writes it into the journal the medium's build reserved, flushes it,
 * reads the slots back and compares them with what it wrote, and only then
 * reports SAVED with the session identifier the report carries.
 *
 * The session is the unit of freshness.  Its identifier is the journal header's
 * persisted counter plus one, so two saves on one medium can never share an
 * identifier; the BEGIN record names it and the build, the END record names the
 * record count and a CRC over the records' text, and the extractor refuses a
 * session whose END is missing or whose CRC does not match.  A torn save is a
 * torn session, never a report that mixes two.
 */

#include "diag_export.h"
#include "serial.h"

#include "ehci.h"
#include "hfoc_build.h"
#include "install_journal_platform.h"
#include "kernel_status_report.h"
#include "xhci.h"

#include <stddef.h>

/* The report's records, composed before anything is written: the slot count and
   the CRC have to be known before the BEGIN/END pair can be committed. */
/*
 * ITEM-582: one byte more than the maximum record, for the terminator.  At
 * exactly DIAG_RECORD_TEXT_MAX the terminator of a full-length record landed
 * in the next row, so the record had no terminator of its own: the read-back
 * compared it against the following record's text (a 440-byte [PANEL] record
 * measured as 521) and a valid save was refused.
 */
static char diag_records[DIAG_REPORT_MAX_RECORDS][DIAG_RECORD_TEXT_MAX + 1U];
/* ITEM-582: true while the current records are a RAM snapshot taken without a
   medium; the console prints them and `diag snapshot` prints them again. */
static bool diag_snapshot_active;
static uint32_t diag_record_count;
static bool diag_record_overflow;
static const char *diag_last_reason = "OK";

/* ITEM-557/558: the refusal the operator is asked about, kept apart from the
   attempt's own state (kernel/src/diag_failure.c) so `diag_reset()` cannot
   erase the answer the next attempt would otherwise overwrite before it is
   read. */
static struct diag_failure diag_failure;

struct diag_composer {
    uint32_t records;
};

uint32_t diag_export_snapshot_records(void) {
    return diag_snapshot_active ? diag_record_count : 0U;
}

const char *diag_export_snapshot_line(uint32_t index) {
    if (!diag_snapshot_active || index >= diag_record_count) {
        return NULL;
    }
    return diag_records[index];
}

const char *diag_export_last_reason(void) {
    return diag_last_reason;
}

const struct diag_failure *diag_export_last_failure(void) {
    return &diag_failure;
}

/*
 * Compose the refusal the operator will read: the stage, the cause, the
 * machine's own answers about the controller, the medium and the journal, and
 * - ITEM-558 - the controller's snapshot of the transfer that failed, so a
 * stage name never stands where the primary cause belongs.
 */
static void diag_fail(enum diag_export_result result, const char *step) {
    struct install_journal_status journal;
    const char *ehci_medium = "UNCHECKED";
    const char *ehci_medium_reason = "NOT_PROBED";
    const char *journal_state = "UNKNOWN";
    const char *journal_reason = "UNKNOWN";
    char commit_text[96];
    char storage_text[192];

    (void)ehci_storage_medium_status(&ehci_medium, &ehci_medium_reason);
    install_journal_platform_status(&journal);
    journal_state = install_journal_state_string(journal.state);
    journal_reason = journal.reason != NULL ? journal.reason : "UNKNOWN";
    diag_failure_set(&diag_failure, diag_export_result_string(result), step,
                     ehci_failure_primary(), ehci_status_string(ehci_status()),
                     ehci_detail(), ehci_medium, ehci_medium_reason,
                     journal_state, journal_reason, ehci_failure_text(),
                     ehci_start_evidence(), ehci_controllers_text());
    /* ITEM-571: the commit point's receipt (header write vs flush, and the
       block result of each) and the bound device's last failed command - its
       opcode, CSW and sense - go into the same line `diag` prints. */
    commit_text[0] = '\0';
    storage_text[0] = '\0';
    (void)install_journal_platform_commit_text(commit_text,
                                               sizeof(commit_text));
    if (!ehci_storage_fault_text(storage_text, sizeof(storage_text))) {
        (void)xhci_storage_fault_text(storage_text, sizeof(storage_text));
    }
    diag_failure_set_evidence(&diag_failure, commit_text, storage_text);
    diag_last_reason = diag_failure.reason;
}

static void diag_reset(void) {
    for (uint32_t index = 0U; index < DIAG_REPORT_MAX_RECORDS; ++index) {
        diag_records[index][0] = '\0';
    }
    diag_record_count = 0U;
    diag_record_overflow = false;
    diag_last_reason = "OK";
    diag_failure_attempt_reset(&diag_failure);
}

static bool diag_store(const char *text, size_t length) {
    if (text == NULL || length == 0U || length > DIAG_RECORD_TEXT_MAX) {
        return false;
    }
    if (diag_record_count >= DIAG_REPORT_MAX_RECORDS) {
        diag_record_overflow = true;
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        diag_records[diag_record_count][index] = text[index];
    }
    diag_records[diag_record_count][length] = '\0';
    ++diag_record_count;
    return true;
}

/* Split one source line into records: the first piece as it is, every later
   piece in front of which DIAG_CONTINUATION says where it came from.  The cut
   is a record boundary, so no fact is lost and no slot is ever over-full. */
static void diag_append_text(const char *text) {
    size_t length;

    if (text == NULL) {
        return;
    }
    for (length = 0U; text[length] != '\0'; ++length) {
    }
    if (length == 0U) {
        return;
    }
    if (length <= DIAG_RECORD_TEXT_MAX) {
        (void)diag_store(text, length);
        return;
    }
    {
        size_t offset = 0U;
        bool first = true;

        while (offset < length) {
            char piece[DIAG_RECORD_TEXT_MAX + 1U];
            size_t capacity = DIAG_RECORD_TEXT_MAX;
            size_t prefix = 0U;
            size_t take;

            if (!first) {
                for (size_t index = 0U; index < DIAG_CONTINUATION_BYTES;
                     ++index) {
                    piece[index] = DIAG_CONTINUATION[index];
                }
                prefix = DIAG_CONTINUATION_BYTES;
            }
            capacity -= prefix;
            take = length - offset;
            if (take > capacity) {
                take = capacity;
            }
            for (size_t index = 0U; index < take; ++index) {
                piece[prefix + index] = text[offset + index];
            }
            piece[prefix + take] = '\0';
            if (!diag_store(piece, prefix + take)) {
                return;
            }
            offset += take;
            first = false;
        }
    }
}

static void diag_sink(void *context, const char *text) {
    (void)context;
    diag_append_text(text);
}

/* The medium's own answer, as one short record: which controller carried it,
   what the identity guard said, and whether the journal is bound. */
static void diag_compose_medium(void) {
    char line[DIAG_REPORT_LINE_MAX];
    struct install_journal_status journal_status;
    const char *ehci_state = "UNCHECKED";
    const char *ehci_reason = "NOT_PROBED";
    const char *xhci_state = "UNCHECKED";
    const char *xhci_reason = "NOT_PROBED";
    const char *journal = "UNBOUND";
    const char *diag_result = "UNAVAILABLE";
    const char *diag_reason = "NOT_PROBED";
    const char *diag_placement = "NONE";
    bool diag_attempted = false;

    (void)ehci_storage_medium_status(&ehci_state, &ehci_reason);
    (void)xhci_storage_medium_status(&xhci_state, &xhci_reason);
    /*
     * ITEM-590: the record names the journal's own state, not a two-way
     * READY/UNBOUND guess.  The installed Toshiba's export said UNBOUND for a
     * journal that had bound its reserved file and simply filled it; a report
     * that cannot tell a full healthy journal from a missing one is the same
     * blind spot that refused the save.
     */
    install_journal_platform_status(&journal_status);
    journal = install_journal_state_string(journal_status.state);
    /* ITEM-582: the separately prepared stick's own answer, so the export says
       whether a diagnostic medium was seen and why it was or was not bound. */
    install_journal_platform_diagnostic_state(&diag_attempted, &diag_result,
                                              &diag_reason,
                                              &diag_placement);
    if (!diag_attempted) {
        diag_result = "UNAVAILABLE";
        diag_reason = "NOT_PROBED";
    }
    {
        struct diag_composer composer = {0U};
        static const char head[] = "[DIAG] medium ehci=";
        static const char middle[] = " xhci=";
        static const char diag_middle[] = " diagnostic=";
        static const char tail[] = " journal=";
        size_t length = 0U;
        bool overflow = false;

        const char *pieces[] = {head, ehci_state, "/", ehci_reason, middle,
                                xhci_state, "/", xhci_reason, diag_middle,
                                diag_result, "/", diag_reason, tail, journal};
        (void)composer;
        for (size_t index = 0U;
             index < sizeof(pieces) / sizeof(pieces[0]); ++index) {
            const char *piece = pieces[index];

            while (*piece != '\0') {
                if (length + 1U >= sizeof(line)) {
                    overflow = true;
                    break;
                }
                line[length++] = *piece++;
            }
            if (overflow) {
                break;
            }
        }
        if (!overflow) {
            line[length] = '\0';
            diag_append_text(line);
        }
    }
}

/*
 * ITEM-602: the composition half, without the medium.  `diag save` calls this
 * with `expose` false and decides what to do when the journal is missing; the
 * SSH `hfoc diag` stream calls it with `expose` true and frames the records it
 * gets.  A composition that could not hold every record returns 0, so neither
 * caller can publish a truncated report as complete.
 */
static uint32_t diag_compose_records(bool expose) {
    diag_reset();
    diag_snapshot_active = false;
    diag_compose_medium();
    xhci_diagnostics_report(diag_sink, NULL);
    ehci_diagnostics_report(diag_sink, NULL);
    kernel_production_status_report(diag_sink, NULL);
    if (diag_record_overflow) {
        return 0U;
    }
    diag_snapshot_active = expose;
    return diag_record_count;
}

uint32_t diag_export_snapshot_compose(void) {
    return diag_compose_records(true);
}

/*
 * ITEM-553: the refusal is a diagnosis too - the first hardware run reported
 * `DIAG FAILED reason=NO_MEDIUM` and nothing else, so the detail now names what
 * the walk saw, what the medium guard said and why the journal is unbound.
 * ITEM-557: every refusal names its stage and cause through one call, keeps
 * them past the next attempt's reset, and hands the console a line that fits.
 */
enum diag_export_result diag_export_save(struct diag_export_session *session) {
    char line[DIAG_REPORT_LINE_MAX];
    uint64_t sequence;
    uint32_t id;
    uint32_t crc;
    uint32_t needed;
    uint32_t written;
    uint32_t capacity;
    uint32_t first;
    uint32_t records = 0U;
    uint32_t bytes = 0U;
    size_t length;

    diag_reset();
    if (session != NULL) {
        session->id = 0U;
        session->records = 0U;
        session->bytes = 0U;
    }

    /*
     * 1. A medium, from whichever controller carries it.  The boot walk has
     * already tried; this is the operator's second chance after a device was
     * inserted, or after the walk ran before the controller was ready - and it
     * includes the separately prepared diagnostic stick, which the publish
     * path binds by its own identity rather than by being a FAT volume.
     *
     * ITEM-590: the question is `saveable()`, not `ready()`.  A journal that
     * bound its reserved file and filled every slot it declares refuses
     * records but is exactly the medium this command exists to write to: the
     * rewind below recovers the room.  Asking `ready()` here re-probed the
     * controllers for a medium the machine already had, and the probe's "no"
     * was then read as "no medium".
     */
    if (!install_journal_platform_saveable()) {
        (void)ehci_storage_medium_publish();
        (void)xhci_storage_medium_publish();
    }
    /*
     * 2. The evidence, whatever the medium says.  The installed machine refused
     * the save at MEDIUM_PROBE and returned before the network, panel and
     * entropy records were composed, so the one answer the machine could give
     * was lost with the medium that was supposed to carry it.  The composition
     * reads values only and needs no medium: a refusal keeps every record in
     * RAM (the console prints the snapshot and `diag snapshot` prints it
     * again), and a bound medium carries it into the reserved file.
     */
    diag_reset();
    diag_snapshot_active = false;
    (void)diag_compose_records(false);
    if (!install_journal_platform_saveable()) {
        diag_snapshot_active = true;
        diag_fail(DIAG_EXPORT_NO_MEDIUM, "MEDIUM_PROBE");
        return DIAG_EXPORT_NO_MEDIUM;
    }

    /* 2. The session: the persisted counter plus one. */
    sequence = install_journal_platform_sequence() + 1U;
    id = (uint32_t)(sequence & 0xFFFFFFFFULL);
    /* The counter is the session's identity on the medium and the header is
       its commit point: persist it before the first record of the session. */
    if (!install_journal_platform_commit_sequence(sequence)) {
        diag_fail(DIAG_EXPORT_VERIFY, "SEQUENCE_COMMIT");
        return DIAG_EXPORT_VERIFY;
    }

    /* 3. The records were composed above; the medium's own answer is the one
       that had to wait for it.  The current network and panel-readiness
       evidence (ITEM-581) travels with them and never contains a password, a
       token or random material. */
    if (diag_record_overflow) {
        diag_fail(DIAG_EXPORT_OVERFLOW, "RECORDS");
        return DIAG_EXPORT_OVERFLOW;
    }
    needed = diag_record_count + DIAG_REPORT_RESERVE_SLOTS;
    written = install_journal_platform_written();
    capacity = install_journal_platform_capacity_slots();
    if (capacity != 0U &&
        (written < 1U || written + needed > capacity)) {
        /* The reserved file rings: the new session takes the whole stream and
           the header's commit point names it before the first record lands. */
        if (!install_journal_platform_rewind(sequence)) {
            diag_fail(DIAG_EXPORT_REWIND, "REWIND");
            return DIAG_EXPORT_REWIND;
        }
        written = install_journal_platform_written();
    }

    /* 4. BEGIN, the body, END - each slot written through the placement's own
       commit point. */
    length = diag_export_begin_line(line, sizeof(line), id,
                                    hfoc_build_id(),
                                    hfoc_version_string(), written, capacity);
    if (length == 0U || !install_journal_platform_write(line, length)) {
        diag_fail(DIAG_EXPORT_WRITE, "BEGIN_WRITE");
        return DIAG_EXPORT_WRITE;
    }
    crc = 0xFFFFFFFFU;
    for (uint32_t index = 0U; index < diag_record_count; ++index) {
        const char *record = diag_records[index];
        size_t record_length = 0U;

        while (record[record_length] != '\0') {
            ++record_length;
        }
        crc = diag_export_crc(crc, record, record_length);
        if (!install_journal_platform_write(record, record_length)) {
            diag_fail(DIAG_EXPORT_WRITE, "BODY_WRITE");
            return DIAG_EXPORT_WRITE;
        }
    }
    crc = ~crc;
    length = diag_export_end_line(line, sizeof(line), id, diag_record_count, crc);
    if (length == 0U || !install_journal_platform_write(line, length)) {
        diag_fail(DIAG_EXPORT_WRITE, "END_WRITE");
        return DIAG_EXPORT_WRITE;
    }
    if (!install_journal_platform_flush()) {
        diag_fail(DIAG_EXPORT_WRITE, "FLUSH");
        return DIAG_EXPORT_WRITE;
    }

    /* 5. Read the session back and compare it with what was written: the
       BEGIN's identifier, every body record byte for byte, and the END's count
       and CRC.  Only then is a save a save. */
    written = install_journal_platform_written();
    if (written < needed + 1U) {
        diag_fail(DIAG_EXPORT_VERIFY, "READ_HEAD");
        return DIAG_EXPORT_VERIFY;
    }
    first = written - needed;
    {
        uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES];
        char text[DIAG_RECORD_TEXT_MAX + 1U];
        bool is_begin = false;
        bool is_end = false;
        uint32_t parsed_id = 0U;
        uint32_t parsed_records = 0U;
        uint32_t parsed_crc = 0U;

        if (!install_journal_platform_read_slot(first, slot)) {
            diag_fail(DIAG_EXPORT_VERIFY, "READ_BEGIN");
            return DIAG_EXPORT_VERIFY;
        }
        if (diag_export_slot_text(slot, text, sizeof(text)) == 0U ||
            !diag_export_parse_marker(text, &is_begin, &is_end, &parsed_id,
                                      NULL, NULL) ||
            !is_begin || parsed_id != id) {
            diag_fail(DIAG_EXPORT_VERIFY, "BEGIN_VERIFY");
            return DIAG_EXPORT_VERIFY;
        }
        for (uint32_t index = 0U; index < diag_record_count; ++index) {
            const char *expected = diag_records[index];
            size_t got;

            if (!install_journal_platform_read_slot(first + 1U + index, slot)) {
                diag_fail(DIAG_EXPORT_VERIFY, "READ_BODY");
                return DIAG_EXPORT_VERIFY;
            }
            got = diag_export_slot_text(slot, text, sizeof(text));
            if (got == 0U) {
                diag_fail(DIAG_EXPORT_VERIFY, "BODY_VERIFY");
                return DIAG_EXPORT_VERIFY;
            }
            {
                size_t want = 0U;

                while (expected[want] != '\0') {
                    ++want;
                }
                if (got != want) {
                    /* ITEM-582: name the record the read-back disagreed on;
                       a bare BODY_LENGTH cannot tell a shifted slot from a
                       truncated one. */
                    const uint64_t verify_flags = serial_sequence_begin();

                    serial_write("[DIAG-VERIFY] step=BODY_LENGTH index=");
                    serial_write_u64(index);
                    serial_write(" got=");
                    serial_write_u64(got);
                    serial_write(" want=");
                    serial_write_u64(want);
                    serial_write(" first=");
                    serial_write_u64(first);
                    serial_write("\n");
                    serial_sequence_end(verify_flags);
                    diag_fail(DIAG_EXPORT_VERIFY, "BODY_LENGTH");
                    return DIAG_EXPORT_VERIFY;
                }
                for (size_t at = 0U; at < want; ++at) {
                    if (text[at] != expected[at]) {
                        diag_fail(DIAG_EXPORT_VERIFY, "BODY_TEXT");
                        return DIAG_EXPORT_VERIFY;
                    }
                }
            }
            bytes += (uint32_t)got;
        }
        records = diag_record_count;
        if (!install_journal_platform_read_slot(written - 1U, slot)) {
            diag_fail(DIAG_EXPORT_VERIFY, "READ_END");
            return DIAG_EXPORT_VERIFY;
        }
        if (diag_export_slot_text(slot, text, sizeof(text)) == 0U ||
            !diag_export_parse_marker(text, &is_begin, &is_end, &parsed_id,
                                      &parsed_records, &parsed_crc) ||
            !is_end || parsed_id != id || parsed_records != records ||
            parsed_crc != crc) {
            diag_fail(DIAG_EXPORT_VERIFY, "END_VERIFY");
            return DIAG_EXPORT_VERIFY;
        }
    }
    /* 6. The header's counter is the session's identity on the medium. */
    if (install_journal_platform_sequence() != sequence) {
        diag_fail(DIAG_EXPORT_VERIFY, "SEQUENCE_VERIFY");
        return DIAG_EXPORT_VERIFY;
    }
    if (session != NULL) {
        session->id = id;
        session->records = records;
        session->bytes = bytes;
    }
    diag_last_reason = "OK";
    return DIAG_EXPORT_OK;
}
