/*
 * ITEM-552 host proof: the diagnostic export's session frame, its CRC and the
 * rule that turns a read-back slot into text.
 *
 * The hardware half cannot run on a host, but everything that decides whether a
 * save is a save lives in kernel/src/diag_session.c: the BEGIN/END framing with
 * the session identifier, the CRC over a session's records, and the slot decode
 * that accepts a written slot only when its length and its CRC check out.  The
 * Debian extractor (`scripts/hfoc-diag-read.py`) applies the same two rules, so
 * this proof also pins the values that script computes - a frame this proof
 * accepts is a frame the .txt writer can read, and a torn slot this proof
 * refuses is a slot the extractor refuses.
 */
#include "diag_export.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

/* Build the slot frame the journal core writes: length, CRC over the text,
   text.  This is the reader's and the extractor's contract, not the writer's
   implementation: the proof builds it here so a change on one side is caught. */
static void make_slot(const char *text, uint8_t *slot) {
    const size_t length = strlen(text);
    uint32_t crc = 0xFFFFFFFFU;

    memset(slot, 0, INSTALL_JOURNAL_SLOT_BYTES);
    slot[0] = (uint8_t)(length & 0xFFU);
    slot[1] = (uint8_t)((length >> 8U) & 0xFFU);
    slot[2] = (uint8_t)((length >> 16U) & 0xFFU);
    slot[3] = (uint8_t)((length >> 24U) & 0xFFU);
    for (size_t index = 0U; index < length; ++index) {
        static const uint32_t polynomial = 0xEDB88320U;

        crc ^= (uint8_t)text[index];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ polynomial : crc >> 1U;
        }
    }
    crc = ~crc;
    slot[4] = (uint8_t)(crc & 0xFFU);
    slot[5] = (uint8_t)((crc >> 8U) & 0xFFU);
    slot[6] = (uint8_t)((crc >> 16U) & 0xFFU);
    slot[7] = (uint8_t)((crc >> 24U) & 0xFFU);
    memcpy(&slot[8], text, length);
}

static void test_framing(void) {
    char begin[192];
    char end[192];
    bool is_begin = false;
    bool is_end = false;
    uint32_t id = 0U;
    uint32_t records = 0U;
    uint32_t crc = 0U;

    CHECK(diag_export_begin_line(begin, sizeof(begin), 0x1A2B3C4DU, "6d949c9b",
                                 "0.2.6", 5U, 512U) != 0U,
          "a BEGIN record is composed");
    CHECK(strncmp(begin, "[DIAG] id=1A2B3C4D state=BEGIN", 30) == 0,
          "the identifier is eight upper-case hex digits and the state is BEGIN");
    CHECK(strstr(begin, "build=6d949c9b") != NULL,
          "the build the report came from is in the BEGIN record");
    CHECK(diag_export_parse_marker(begin, &is_begin, &is_end, &id, NULL,
                                   NULL) &&
              is_begin && !is_end && id == 0x1A2B3C4DU,
          "the BEGIN record reads back as its own session");

    CHECK(diag_export_end_line(end, sizeof(end), 0x1A2B3C4DU, 7U,
                               0xC83CFD51U) != 0U &&
              strstr(end, "state=END records=7 crc=C83CFD51") != NULL,
          "an END record names the record count and the session CRC");
    CHECK(diag_export_parse_marker(end, &is_begin, &is_end, &id, &records,
                                   &crc) &&
              is_end && !is_begin && id == 0x1A2B3C4DU && records == 7U &&
              crc == 0xC83CFD51U,
          "the END record reads back with its count and CRC");
    CHECK(!diag_export_parse_marker("[DIAG] something else", &is_begin,
                                    &is_end, &id, NULL, NULL),
          "a record that is not a session marker is not one");
    CHECK(!diag_export_parse_marker("[DIAG] id=XYZ state=BEGIN", &is_begin,
                                    &is_end, &id, NULL, NULL),
          "a marker with a malformed identifier is refused");
    CHECK(!diag_export_parse_marker(
              "[DIAG] id=1A2B3C4D state=END records=7 crc=1234", &is_begin,
              &is_end, &id, &records, &crc),
          "an END record with a short CRC is refused");

    /* A short buffer gets no line at all rather than half of one. */
    {
        char small[8];

        CHECK(diag_export_begin_line(small, sizeof(small), 1U, "b", "v", 1U,
                                     2U) == 0U &&
                  small[0] == '\0',
              "a BEGIN record that does not fit is refused, not cut");
    }
}

static void test_crc(void) {
    /* The standard CRC-32 the journal core and the Debian reader use: the
       values below are zlib.crc32() of the same byte streams. */
    CHECK((~diag_export_crc(0xFFFFFFFFU, "abc", 3U)) == 0x4788814EU,
          "the session CRC is the standard CRC-32 of the text and a newline");
    CHECK((~diag_export_crc(0xFFFFFFFFU, "", 0U)) == 0x32D70693U,
          "an empty record still contributes its newline");
    CHECK((~diag_export_crc(
               0xFFFFFFFFU,
               "[DIAG] id=00000001 state=BEGIN build=x version=y slots=1/100",
               60U)) == 0xC83CFD51U,
          "the CRC matches zlib.crc32 over the whole record");
    {
        uint32_t running = 0xFFFFFFFFU;

        running = diag_export_crc(running, "one", 3U);
        running = diag_export_crc(running, "two", 3U);
        CHECK((~running) == 0x12BA0BE6U,
              "the CRC of two records is the CRC of their concatenation");
    }
}

static void test_slot_decode(void) {
    uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES];
    char text[DIAG_RECORD_TEXT_MAX + 1U];

    make_slot("[DIAG] id=00000001 state=BEGIN", slot);
    CHECK(diag_export_slot_text(slot, text, sizeof(text)) == 30U &&
              strcmp(text, "[DIAG] id=00000001 state=BEGIN") == 0,
          "a written slot reads back as its own text");

    make_slot("[DIAG] id=00000001 state=BEGIN", slot);
    slot[9] ^= 0x01U;
    CHECK(diag_export_slot_text(slot, text, sizeof(text)) == 0U,
          "a slot whose text no longer matches its CRC is torn");

    memset(slot, 0, sizeof(slot));
    CHECK(diag_export_slot_text(slot, text, sizeof(text)) == 0U,
          "an empty slot is not a record");

    make_slot("[DIAG] id=00000001 state=BEGIN", slot);
    slot[0] = 0xFFU;
    slot[1] = 0xFFU;
    slot[3] = 0x00U;
    CHECK(diag_export_slot_text(slot, text, sizeof(text)) == 0U,
          "a slot whose length cannot be a record is refused");

    make_slot("[DIAG] id=00000001 state=BEGIN", slot);
    CHECK(diag_export_slot_text(slot, text, 8U) == 0U && text[0] == '\0',
          "a caller buffer that cannot hold the text is refused");
}

static void test_result_names(void) {
    CHECK(strcmp(diag_export_result_string(DIAG_EXPORT_OK), "OK") == 0,
          "a verified save has its own name");
    CHECK(strcmp(diag_export_result_string(DIAG_EXPORT_NO_MEDIUM),
                 "NO_MEDIUM") == 0,
          "a machine with no reachable medium says so");
    CHECK(strcmp(diag_export_result_string(DIAG_EXPORT_VERIFY), "VERIFY") == 0,
          "a read-back that did not match is its own refusal");
    CHECK(strcmp(diag_export_result_string(DIAG_EXPORT_FULL), "FULL") == 0,
          "a journal with no room left is named");
}

/*
 * ITEM-558: the refusal record survives the attempt's cleanup, and its short
 * line fits the row the operator reads.
 *
 * The hardware run reported `ehci=DESCRIPTOR_FAILED/DEVICE8` and no transfer:
 * the stage was in the record, the primary cause was not.  The first case feeds
 * the record exactly what the real walk and the driver produce - the stage and
 * the controller's snapshot - then calls the production cleanup, and requires
 * both to still be readable.  The second case pins the row bound: fields that
 * would cross it are dropped whole, never shown half.
 */
static void test_failure_record_survives_cleanup(void) {
    struct diag_failure failure;
    char line[DIAG_FAILURE_ROW_MAX + 1U];
    char detail[DIAG_FAILURE_DETAIL_MAX];
    size_t length;

    memset(&failure, 0, sizeof(failure));
    diag_failure_set(
        &failure, "NO_MEDIUM", "MEDIUM_PROBE", "XACTERR", "DESCRIPTOR_FAILED",
        "DEVICE8", "UNCHECKED", "NOT_PROBED", "UNBOUND", "NONE",
        "pci=00:1D.0 port=1 result=XACTERR index=0/3 qtd=0x00000048 "
        "addr=0 ep=0x80 hub=0 hub_port=0 received=0 residual=8",
        "pci=00:1A.0 cap=0x00000020 legacy=TAKEN pcicmd=0x00000006 "
        "pse=1 pss=1 clr=0x00000020 clr_rb=0x00000020 pss_stop=1 "
        "start=0x00010030/0x00010030 hse=NONE cause=OK",
        "00:1A.0=PORT_NOT_ENABLED/ROOT_RESET;00:1D.0=NO_DEVICE/NO_CONNECTED_PORT");
    diag_failure_attempt_reset(&failure);

    CHECK(failure.recorded, "the refusal stays recorded after the cleanup");
    CHECK(strcmp(failure.reason, "NO_MEDIUM") == 0 &&
              strcmp(failure.step, "MEDIUM_PROBE") == 0 &&
              strcmp(failure.primary, "XACTERR") == 0,
          "the cleanup keeps the cause, the stage and the primary result");

    length = diag_failure_short_line(&failure, line, sizeof(line));
    CHECK(strcmp(line, "DIAG FAILED reason=NO_MEDIUM step=MEDIUM_PROBE "
                       "primary=XACTERR") == 0,
          "the short line names the cause, the stage and the primary result");
    CHECK(length == strlen(line) && length <= DIAG_FAILURE_ROW_MAX,
          "the short line fits one 80-column row");

    length = diag_failure_detail_line(&failure, detail, sizeof(detail));
    CHECK(length != 0U && strstr(detail, "step=MEDIUM_PROBE") != NULL &&
              strstr(detail, "cause=NO_MEDIUM") != NULL &&
              strstr(detail, "ehci=DESCRIPTOR_FAILED/DEVICE8") != NULL &&
              strstr(detail, "result=XACTERR") != NULL &&
              strstr(detail, "qtd=0x00000048") != NULL,
          "the detail carries the stage and the transfer's primary cause");
    /* ITEM-561: an early refusal writes no report, so the start evidence has
       to fit in the record `diag` prints. */
    CHECK(strstr(detail, "ehci_start=pci=00:1A.0") != NULL &&
              strstr(detail, "pse=1 pss=1") != NULL &&
              strstr(detail, "start=0x00010030/0x00010030") != NULL,
          "the detail carries the start evidence of the refused controller");
    /* ITEM-562: both controllers, each with its own answer. */
    CHECK(strstr(detail, "ehci_all=00:1A.0=PORT_NOT_ENABLED/ROOT_RESET") != NULL &&
              strstr(detail, "00:1D.0=NO_DEVICE/NO_CONNECTED_PORT") != NULL,
          "the detail carries every controller's own verdict");
    CHECK(length < sizeof(detail) && length + 1U <= DIAG_FAILURE_DETAIL_MAX,
          "the start evidence still fits the record's bound");
}

static void test_failure_short_line_drops_whole_fields(void) {
    struct diag_failure failure;
    char line[DIAG_FAILURE_ROW_MAX + 1U];
    size_t length;

    memset(&failure, 0, sizeof(failure));
    diag_failure_set(&failure, "VERY_LONG_CAUSE_CODE_XX",
                     "VERY_LONG_STAGE_CODE_XXXX", "VERY_LONG_PRIMARY_XXXX",
                     "S", "D", "M", "R", "J", "R", NULL, NULL, NULL);
    length = diag_failure_short_line(&failure, line, sizeof(line));
    diag_failure_attempt_reset(&failure);

    CHECK(length <= DIAG_FAILURE_ROW_MAX, "the short line never crosses the row");
    CHECK(failure.recorded, "the record stays after the cleanup");
    CHECK(strstr(line, "primary=") == NULL,
          "a field that does not fit is dropped whole, not shown half");
    CHECK(line[length] == '\0' && length > 0U,
          "the line is a terminated whole-field prefix");
    CHECK(diag_failure_short_line(&failure, NULL, 0U) == 0U,
          "a null buffer gets no line");
}

static void test_no_failure_means_no_line(void) {
    struct diag_failure failure;
    char line[DIAG_FAILURE_ROW_MAX + 1U];

    memset(&failure, 0, sizeof(failure));
    diag_failure_attempt_reset(&failure);
    CHECK(!failure.recorded, "a machine with no refusal has no record");
    CHECK(diag_failure_short_line(&failure, line, sizeof(line)) == 0U,
          "and prints no refusal line");
}


/* ITEM-571: the refusal carries the commit point's receipt and the bound
   device's last failed command, in the detail `diag` prints - both are RAM
   snapshots, so a machine whose journal medium is the failure still prints
   them. */
static void test_failure_carries_the_commit_evidence(void) {
    struct diag_failure failure;
    char detail[DIAG_FAILURE_DETAIL_MAX];
    size_t length;

    memset(&failure, 0, sizeof(failure));
    diag_failure_set(&failure, "VERIFY", "SEQUENCE_COMMIT", "XACTERR",
                     "HOST_SYSTEM_ERROR", "HSE", "MATCH", "OK", "BOUND", "OK",
                     "pci=00:1A.0 port=1 result=TIMEOUT", NULL,
                     "00:1A.0=OK/BOUND");
    diag_failure_set_evidence(&failure, "FLUSH blk=IO_ERROR",
                              "op=35/COMMAND_FAILED FLUSH csw=1 res=0 "
                              "sense=OK/70-03-11-04");
    length = diag_failure_detail_line(&failure, detail, sizeof(detail));
    CHECK(length != 0U && strstr(detail, "commit=FLUSH blk=IO_ERROR") != NULL,
          "the detail names the commit step that failed and its block result");
    CHECK(strstr(detail, "storage=op=35/COMMAND_FAILED") != NULL,
          "the detail names the device's own failed command");
    CHECK(strstr(detail, "70-03-11-04") != NULL,
          "the detail carries the sense data the device answered");
}

int main(void) {
    test_framing();
    test_crc();
    test_failure_record_survives_cleanup();
    test_failure_short_line_drops_whole_fields();
    test_no_failure_means_no_line();
    test_slot_decode();
    test_result_names();
    test_failure_carries_the_commit_evidence();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("diagnostic export session host test: ALL OK");
    return 0;
}
