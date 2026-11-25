/*
 * ITEM-521 (increment 11): host proof for the panic records the install
 * journal has to carry.
 *
 * Requirement 1 of the owner's item names the panic among the records the
 * medium's journal must hold, and requirement 2 asks the panic path to get its
 * tail out.  The path already flushed what was staged *before* the machine
 * stopped, but the panic itself was never a record: it went to the serial
 * stream and the screen only.  This witness pins the two renderers - the exact
 * text of the message record (with and without a frame), the exact text of the
 * detail record, the clamp that marks an over-long message instead of hiding
 * it, the too-small buffer (nothing written, an empty answer instead of a
 * truncated record) and the bytes around the output staying untouched - and it
 * pins that both records fit the shared record bound, so neither the durable
 * log nor the journal clamps them on the way to the medium.
 */

#include "panic_record.h"

#include "appliance_boot_log.h"
#include "install_journal_core.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
static unsigned failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));          \
        }                                                                    \
    } while (0)

#define CANARY 0xA5U
#define CANARY_BYTES 8U

static void fill_canary(unsigned char *buffer, size_t length) {
    memset(buffer, CANARY, length);
}

static bool canary_intact(const unsigned char *buffer, size_t from,
                          size_t length) {
    for (size_t index = from; index < length; ++index) {
        if (buffer[index] != CANARY) {
            return false;
        }
    }
    return true;
}

/* A message of exactly `length` bytes, all 'm', for the clamp boundaries. */
static void fill_message(char *message, size_t length) {
    memset(message, 'm', length);
    message[length] = '\0';
}

int main(void) {
    /* 1. The message record the panic path stages: one line, the message and
          the frame facts the screen and the serial stream already carry. */
    {
        char record[PANIC_RECORD_MAX];
        const size_t length = panic_record_render(
            record, sizeof(record), "Divide error", true, 0U, 0U,
            0xDEADBEEFU);
        CHECK(strcmp(record,
                     "[PANIC] Divide error vector=0 "
                     "error=0x0000000000000000 rip=0x00000000DEADBEEF") == 0,
              "the panic record names the message and the frame");
        CHECK(length == strlen(record),
              "the returned length is the rendered length");
        CHECK(strchr(record, '\n') == NULL,
              "the record is one line without a transport newline");
    }

    /* 2. A panic without a frame - the console's own `panic` command - carries
          no invented vector: the message is the whole record. */
    {
        char record[PANIC_RECORD_MAX];
        const size_t length = panic_record_render(
            record, sizeof(record), "Panic requested from console", false, 0U,
            0U, 0U);
        CHECK(strcmp(record, "[PANIC] Panic requested from console") == 0,
              "a frameless panic record carries the message alone");
        CHECK(length == strlen(record),
              "the frameless record length is the rendered length");
    }

    /* 3. A null message is the same words the serial stream prints. */
    {
        char record[PANIC_RECORD_MAX];
        CHECK(panic_record_render(record, sizeof(record), NULL, false, 0U, 0U,
                                  0U) == strlen("[PANIC] Unknown panic") &&
                  strcmp(record, "[PANIC] Unknown panic") == 0,
              "a null message renders as Unknown panic");
    }

    /* 4. The frame fields keep their full width: the widest vector, error and
          rip are rendered in decimal and in the serial stream's hex shape. */
    {
        char record[PANIC_RECORD_MAX];
        CHECK(panic_record_render(record, sizeof(record), "Double fault", true,
                                  UINT64_MAX, UINT64_MAX, UINT64_MAX) ==
                  strlen("[PANIC] Double fault "
                         "vector=18446744073709551615 "
                         "error=0xFFFFFFFFFFFFFFFF rip=0xFFFFFFFFFFFFFFFF") &&
                  strcmp(record,
                         "[PANIC] Double fault "
                         "vector=18446744073709551615 "
                         "error=0xFFFFFFFFFFFFFFFF rip=0xFFFFFFFFFFFFFFFF") ==
                      0,
              "the frame fields render the boundary values in full");
    }

    /* 5. A message longer than the bound is clamped and marked, never dropped
          and never silently shortened (requirement 5). */
    {
        char message[PANIC_RECORD_MESSAGE_MAX + 2U];
        char record[PANIC_RECORD_MAX];
        size_t length;

        fill_message(message, PANIC_RECORD_MESSAGE_MAX);
        length = panic_record_render(record, sizeof(record), message, false, 0U,
                                     0U, 0U);
        CHECK(length == strlen("[PANIC] ") + PANIC_RECORD_MESSAGE_MAX,
              "a message at the bound is rendered whole");
        CHECK(strcmp(record + length - 3U, "mmm") == 0,
              "a message at the bound keeps its own bytes");

        fill_message(message, PANIC_RECORD_MESSAGE_MAX + 1U);
        length = panic_record_render(record, sizeof(record), message, false, 0U,
                                     0U, 0U);
        CHECK(length == strlen("[PANIC] ") + PANIC_RECORD_MESSAGE_MAX,
              "an over-long message is clamped to the bound");
        CHECK(strcmp(record + length - 3U, "...") == 0,
              "the clamp is marked with a visible tail");
    }

    /* 6. The detail record: the second half of a panic, as the serial stream
          and the screen print it. */
    {
        char record[PANIC_RECORD_DETAIL_LINE_MAX];
        const size_t length = panic_record_detail(
            record, sizeof(record),
            "page-fault: cause=not-present access=write mode=supervisor");
        CHECK(strcmp(record,
                     "[PANIC] detail=page-fault: cause=not-present "
                     "access=write mode=supervisor") == 0,
              "the detail record names the panic detail");
        CHECK(length == strlen(record),
              "the detail length is the rendered length");
    }

    /* 7. No detail, no record: the panic path stages nothing for a panic that
          never had a detail line. */
    {
        char record[PANIC_RECORD_DETAIL_LINE_MAX];
        record[0] = 'x';
        CHECK(panic_record_detail(record, sizeof(record), NULL) == 0U &&
                  record[0] == '\0',
              "a null detail is refused with an emptied buffer");
        record[0] = 'x';
        CHECK(panic_record_detail(record, sizeof(record), "") == 0U &&
                  record[0] == '\0',
              "an empty detail is refused with an emptied buffer");
    }

    /* 8. The detail has the same clamp rule as the message. */
    {
        char detail[PANIC_RECORD_DETAIL_MAX + 2U];
        char record[PANIC_RECORD_DETAIL_LINE_MAX];
        size_t length;

        fill_message(detail, PANIC_RECORD_DETAIL_MAX);
        length = panic_record_detail(record, sizeof(record), detail);
        CHECK(length == strlen("[PANIC] detail=") + PANIC_RECORD_DETAIL_MAX,
              "a detail at the bound is rendered whole");
        CHECK(strcmp(record + length - 3U, "mmm") == 0,
              "a detail at the bound keeps its own bytes");

        fill_message(detail, PANIC_RECORD_DETAIL_MAX + 1U);
        length = panic_record_detail(record, sizeof(record), detail);
        CHECK(length == strlen("[PANIC] detail=") + PANIC_RECORD_DETAIL_MAX,
              "an over-long detail is clamped to the bound");
        CHECK(strcmp(record + length - 3U, "...") == 0,
              "the detail clamp is marked with a visible tail");
    }

    /* 9. A buffer that cannot hold the whole record is refused, and the
          refusal empties the buffer instead of leaving a fragment in it. */
    {
        unsigned char buffer[PANIC_RECORD_MAX + CANARY_BYTES];
        char *const record = (char *)buffer;
        size_t length;

        fill_canary(buffer, sizeof(buffer));
        length = panic_record_render(record, PANIC_RECORD_MAX, "Divide error",
                                     true, 0U, 0U, 0x401234U);
        CHECK(length != 0U && length + 1U <= PANIC_RECORD_MAX,
              "the witness record fits its own bound");
        CHECK(canary_intact(buffer, PANIC_RECORD_MAX, sizeof(buffer)),
              "the render writes inside the declared bound");

        fill_canary(buffer, sizeof(buffer));
        CHECK(panic_record_render(record, length + 1U, "Divide error", true, 0U,
                                  0U, 0x401234U) == length &&
                  strcmp(record, "[PANIC] Divide error vector=0 "
                                 "error=0x0000000000000000 "
                                 "rip=0x0000000000401234") == 0,
              "a buffer of exactly the record plus its NUL is accepted");

        fill_canary(buffer, sizeof(buffer));
        CHECK(panic_record_render(record, length, "Divide error", true, 0U, 0U,
                                  0x401234U) == 0U &&
                  record[0] == '\0',
              "a buffer one byte short is refused");
        CHECK(canary_intact(buffer, PANIC_RECORD_MAX, sizeof(buffer)),
              "the refused record writes nothing past the buffer");
    }

    /* 10. The same refusal rule for the detail record and for a caller that
           hands over nothing at all. */
    {
        const char *const detail = "page-fault: cause=not-present";
        const size_t needed = strlen("[PANIC] detail=") + strlen(detail) + 1U;
        char record[PANIC_RECORD_DETAIL_LINE_MAX];

        record[0] = 'x';
        CHECK(panic_record_detail(record, needed - 1U, detail) == 0U &&
                  record[0] == '\0',
              "a detail record one byte short is refused");
        CHECK(panic_record_detail(record, needed, detail) == needed - 1U,
              "a detail record of exactly its own size is accepted");
        record[0] = 'x';
        CHECK(panic_record_detail(record, 0U, detail) == 0U && record[0] == 'x',
              "a zero capacity writes nothing at all");
        CHECK(panic_record_detail(NULL, sizeof(record), detail) == 0U,
              "a null buffer is refused");
        CHECK(panic_record_render(NULL, PANIC_RECORD_MAX, "Divide error", true,
                                  0U, 0U, 0U) == 0U,
              "a null message buffer is refused");
        CHECK(panic_record_render(record, 0U, "Divide error", true, 0U, 0U,
                                  0U) == 0U,
              "a zero capacity message record is refused");
    }

    /* 11. The record is NUL-terminated where the length says it is, and the
           same panic renders the same bytes twice. */
    {
        char first[PANIC_RECORD_MAX];
        char second[PANIC_RECORD_MAX];
        const size_t length = panic_record_render(
            first, sizeof(first), "Page fault", true, 14U, 0x2U,
            0xFFFFFFFFDEADBEEFU);

        CHECK(length + 1U <= sizeof(first) && first[length] == '\0',
              "the record is NUL-terminated at its length");
        CHECK(panic_record_render(second, sizeof(second), "Page fault", true,
                                  14U, 0x2U, 0xFFFFFFFFDEADBEEFU) == length &&
                  memcmp(first, second, length + 1U) == 0,
              "the same panic renders the same record");
    }

    /* 12. Neither record can be clamped downstream: both fit the bound the
           durable log and the journal share. */
    {
        CHECK(PANIC_RECORD_MAX <= APPLIANCE_BOOT_LOG_TEXT_MAX &&
                  PANIC_RECORD_DETAIL_LINE_MAX <= APPLIANCE_BOOT_LOG_TEXT_MAX,
              "both panic records fit the durable log's record bound");
        CHECK(PANIC_RECORD_MAX <= INSTALL_JOURNAL_TEXT_MAX &&
                  PANIC_RECORD_DETAIL_LINE_MAX <= INSTALL_JOURNAL_TEXT_MAX,
              "both panic records fit the journal's record bound");
    }

    printf("panic-record-host: %u checks, %u failures\n", checks, failures);
    return failures == 0U ? 0 : 1;
}
