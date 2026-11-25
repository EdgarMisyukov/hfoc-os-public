#include "appliance_ssh_command.h"

#include "appliance_boot_log_store.h"
#include "diag_export.h"
#include "hfoc_build.h"
#include "install_journal_platform.h"
#include "kernel_status_report.h"

#include <string.h>

/*
 * ITEM-601/602: the four approved diagnostics, answered through the owners
 * that already compose them.
 *
 * The channel never parses a shell line: the dispatcher compares the whole
 * command with four literals and refuses every other byte sequence.  The sink
 * is bounded; a composition that does not fit is a named refusal with a
 * nonzero status, never a truncated report shown as complete.
 */

#define HFOC_SSH_JOURNAL_RECORDS 32U
#define HFOC_SSH_COMMAND_MAX_LENGTH 64U

enum appliance_ssh_command_kind {
    APPLIANCE_SSH_COMMAND_ALL = 0,
    APPLIANCE_SSH_COMMAND_NETWORK,
};

struct appliance_ssh_command_sink {
    uint8_t *output;
    size_t capacity;
    size_t length;
    enum appliance_ssh_command_kind kind;
    bool overflow;
    bool refused;
};

static void appliance_ssh_command_append(
    struct appliance_ssh_command_sink *sink,
    const char *text,
    size_t length)
{
    if (sink->overflow) {
        return;
    }
    if (sink->length > sink->capacity ||
        length > sink->capacity - sink->length) {
        sink->overflow = true;
        return;
    }
    memcpy(sink->output + sink->length, text, length);
    sink->length += length;
}

static bool appliance_ssh_command_prefix(const char *line, const char *prefix)
{
    const size_t prefix_length = strlen(prefix);

    return line != NULL && strncmp(line, prefix, prefix_length) == 0;
}

static bool appliance_ssh_command_line_wanted(
    const struct appliance_ssh_command_sink *sink,
    const char *line)
{
    if (sink->kind == APPLIANCE_SSH_COMMAND_ALL) {
        return true;
    }
    return appliance_ssh_command_prefix(line, "[NET-") ||
           appliance_ssh_command_prefix(line, "[PANEL]") ||
           appliance_ssh_command_prefix(line, "[PRODUCTION-SSH]");
}

static void appliance_ssh_command_sink_line(void *context, const char *line)
{
    struct appliance_ssh_command_sink *sink = context;

    if (line == NULL || !appliance_ssh_command_line_wanted(sink, line)) {
        return;
    }
    appliance_ssh_command_append(sink, line, strlen(line));
    appliance_ssh_command_append(sink, "\n", 1U);
}

static void appliance_ssh_command_journal_line(void *context, const char *text,
                                               size_t length)
{
    struct appliance_ssh_command_sink *sink = context;

    if (text == NULL || length == 0U) {
        return;
    }
    appliance_ssh_command_append(sink, text, length);
    if (text[length - 1U] != '\n') {
        appliance_ssh_command_append(sink, "\n", 1U);
    }
}

static void appliance_ssh_command_append_u32(
    struct appliance_ssh_command_sink *sink,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) {
        char one[2];

        one[0] = digits[--count];
        one[1] = '\0';
        appliance_ssh_command_append(sink, one, 1U);
    }
}

static void appliance_ssh_command_append_hex32(
    struct appliance_ssh_command_sink *sink,
    uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char text[8];

    for (uint32_t nibble = 0U; nibble < 8U; ++nibble) {
        text[nibble] = digits[(value >> (28U - nibble * 4U)) & 0x0FU];
    }
    appliance_ssh_command_append(sink, text, sizeof(text));
}

const char *appliance_ssh_command_refusal(void)
{
    return "hfoc: refused: command not available over SSH in this build\n";
}

static bool appliance_ssh_command_is(const char *command, size_t length,
                                     const char *expected)
{
    const size_t expected_length = strlen(expected);

    return length == expected_length &&
           memcmp(command, expected, expected_length) == 0;
}

static bool appliance_ssh_command_text_valid(const char *command,
                                             size_t length)
{
    size_t index;

    if (command == NULL || length == 0U ||
        length > HFOC_SSH_COMMAND_MAX_LENGTH) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        const unsigned char byte = (unsigned char)command[index];

        /*
         * The approved commands are printable ASCII.  A control byte, a
         * non-ASCII byte (an invalid/unauthorized UTF-8 sequence), NUL, a
         * newline or the shell separator cannot be part of one, so it is
         * refused before the exact comparison rather than being ignored.
         */
        if (byte < 0x20U || byte > 0x7EU || byte == ';') {
            return false;
        }
    }
    return true;
}

static bool appliance_ssh_command_refuse(
    struct appliance_ssh_command_sink *sink,
    const char *reason)
{
    static const char prefix[] = "hfoc: refused: ";

    sink->length = 0U;
    sink->overflow = false;
    sink->refused = true;
    appliance_ssh_command_append(sink, prefix, sizeof(prefix) - 1U);
    appliance_ssh_command_append(sink, reason, strlen(reason));
    appliance_ssh_command_append(sink, "\n", 1U);
    if (sink->overflow) {
        sink->length = 0U;
        return false;
    }
    return true;
}

static bool appliance_ssh_command_status(
    struct appliance_ssh_command_sink *sink)
{
    kernel_production_status_report(appliance_ssh_command_sink_line, sink);
    return !sink->overflow;
}

static bool appliance_ssh_command_network(
    struct appliance_ssh_command_sink *sink)
{
    static const char empty[] = "hfoc: network: no record\n";

    sink->kind = APPLIANCE_SSH_COMMAND_NETWORK;
    kernel_production_status_report(appliance_ssh_command_sink_line, sink);
    if (!sink->overflow && sink->length == 0U) {
        appliance_ssh_command_append(sink, empty, sizeof(empty) - 1U);
    }
    return !sink->overflow;
}

static bool appliance_ssh_command_journal(
    struct appliance_ssh_command_sink *sink)
{
    (void)appliance_boot_log_store_read_records(
        appliance_ssh_command_journal_line, sink, HFOC_SSH_JOURNAL_RECORDS);
    return !sink->overflow;
}

static bool appliance_ssh_command_diag(struct appliance_ssh_command_sink *sink)
{
    static const char stream_line[] =
        "[DIAG] transport=ssh mode=STREAM source=RAM verified=COMPOSED"
        " saved=no records=";
    char begin[DIAG_REPORT_LINE_MAX];
    char end[DIAG_REPORT_LINE_MAX];
    uint32_t count = diag_export_snapshot_compose();
    uint32_t id;
    uint32_t crc = 0xFFFFFFFFU;
    size_t length;

    if (count == 0U) {
        return appliance_ssh_command_refuse(sink, "diag composition overflow");
    }
    for (uint32_t index = 0U; index < count; ++index) {
        const char *record = diag_export_snapshot_line(index);

        if (record == NULL) {
            (void)diag_export_snapshot_compose();
            return appliance_ssh_command_refuse(
                sink, "diag composition is torn");
        }
        crc = diag_export_crc(crc, record, strlen(record));
    }
    crc = ~crc;
    id = (uint32_t)(install_journal_platform_sequence() + 1U);
    length = diag_export_begin_line(
        begin, sizeof(begin), id, hfoc_build_id(), hfoc_version_string(),
        install_journal_platform_written(),
        install_journal_platform_capacity_slots());
    if (length == 0U) {
        return appliance_ssh_command_refuse(sink, "diag begin framing");
    }
    appliance_ssh_command_append(sink, begin, length);
    appliance_ssh_command_append(sink, "\n", 1U);
    for (uint32_t index = 0U; index < count; ++index) {
        const char *record = diag_export_snapshot_line(index);

        if (record == NULL) {
            return appliance_ssh_command_refuse(sink,
                                                "diag composition is torn");
        }
        appliance_ssh_command_append(sink, record, strlen(record));
        appliance_ssh_command_append(sink, "\n", 1U);
    }
    length = diag_export_end_line(end, sizeof(end), id, count, crc);
    if (length == 0U) {
        return appliance_ssh_command_refuse(sink, "diag end framing");
    }
    appliance_ssh_command_append(sink, end, length);
    appliance_ssh_command_append(sink, "\n", 1U);
    /*
     * The stream is a complete composition, not a medium save: the shared
     * reader verifies the count and CRC above, and this line says which half
     * of the session actually ran.
     */
    appliance_ssh_command_append(sink, stream_line, sizeof(stream_line) - 1U);
    appliance_ssh_command_append_u32(sink, count);
    appliance_ssh_command_append(sink, " crc=", 5U);
    appliance_ssh_command_append_hex32(sink, crc);
    appliance_ssh_command_append(sink, "\n", 1U);
    return !sink->overflow;
}

bool appliance_ssh_command_dispatch(
    const char *command,
    size_t command_length,
    uint8_t *output,
    size_t capacity,
    size_t *output_length,
    int *status)
{
    struct appliance_ssh_command_sink sink;
    const char *refusal = appliance_ssh_command_refusal();
    bool ok;

    if (output == NULL || output_length == NULL || status == NULL ||
        capacity == 0U) {
        return false;
    }
    *output_length = 0U;
    *status = 127;
    if (!appliance_ssh_command_text_valid(command, command_length)) {
        return false;
    }
    memset(&sink, 0, sizeof(sink));
    sink.output = output;
    sink.capacity = capacity;
    sink.kind = APPLIANCE_SSH_COMMAND_ALL;

    if (appliance_ssh_command_is(command, command_length, "hfoc status")) {
        ok = appliance_ssh_command_status(&sink);
    } else if (appliance_ssh_command_is(command, command_length,
                                        "hfoc network")) {
        ok = appliance_ssh_command_network(&sink);
    } else if (appliance_ssh_command_is(command, command_length,
                                        "hfoc journal")) {
        ok = appliance_ssh_command_journal(&sink);
    } else if (appliance_ssh_command_is(command, command_length,
                                        "hfoc diag")) {
        ok = appliance_ssh_command_diag(&sink);
    } else {
        const size_t refusal_length = strlen(refusal);

        if (refusal_length > capacity) {
            return false;
        }
        memcpy(output, refusal, refusal_length);
        *output_length = refusal_length;
        return true;
    }
    if (!ok) {
        /*
         * The sink may have written a partial body before the overflow; a
         * refusal replaces it whole so the client never reads half a report
         * as if it were the answer.
         */
        if (appliance_ssh_command_refuse(&sink,
                                         "output exceeds the SSH bound")) {
            *output_length = sink.length;
            *status = 127;
            return true;
        }
        return false;
    }
    *output_length = sink.length;
    *status = sink.refused ? 127 : 0;
    return true;
}
