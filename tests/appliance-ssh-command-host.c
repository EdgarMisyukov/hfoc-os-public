/*
 * ITEM-602: the SSH command surface over the real dispatch.
 *
 * The QEMU proof drives the production owners; this host proof pins the
 * allowlist, the bounded sink, the diag session framing and the refusal rules
 * with the same `appliance_ssh_command_dispatch()` the production endpoint
 * calls.  The owner functions below are deterministic stand-ins so a failure
 * names the dispatch rule that broke, not a hardware path.
 */
#include "appliance_ssh_command.h"
#include "appliance_boot_log_store.h"
#include "diag_export.h"
#include "kernel_status_report.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...)                                                 \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL: ");                                        \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

void kernel_production_status_report(kernel_status_sink_fn sink, void *context)
{
    if (sink == NULL) {
        return;
    }
    sink(context, "[NET-STATUS] device=virtio readiness=READY link=UP");
    sink(context, "[NET-ADDR] state=ACTIVE address=192.0.2.13");
    sink(context, "[PANEL] state=READY listener=up");
    sink(context, "[PRODUCTION-SSH] state=LISTENING port=22");
    sink(context, "[RNG] source=RDSEED state=READY");
    sink(context, "[ADMIN] state=ACTIVE last=OK");
}

static const char *const diag_lines[] = {
    "[NET-STATUS] device=virtio readiness=READY link=UP",
    "[PANEL] state=READY listener=up",
    "[RNG] source=RDSEED state=READY",
};
#define DIAG_LINE_COUNT \
    ((uint32_t)(sizeof(diag_lines) / sizeof(diag_lines[0])))

static uint32_t diag_extra_records;

uint32_t diag_export_snapshot_compose(void)
{
    return diag_extra_records != 0U ? diag_extra_records : DIAG_LINE_COUNT;
}

uint32_t diag_export_snapshot_records(void)
{
    return diag_extra_records != 0U ? diag_extra_records : DIAG_LINE_COUNT;
}

const char *diag_export_snapshot_line(uint32_t index)
{
    static char overflow_line[256];

    if (diag_extra_records != 0U) {
        memset(overflow_line, 'x', sizeof(overflow_line) - 1U);
        overflow_line[sizeof(overflow_line) - 1U] = '\0';
        return overflow_line;
    }
    if (index >= DIAG_LINE_COUNT) {
        return NULL;
    }
    return diag_lines[index];
}

uint64_t install_journal_platform_sequence(void)
{
    return 41U;
}

uint32_t install_journal_platform_written(void)
{
    return 0U;
}

uint32_t install_journal_platform_capacity_slots(void)
{
    return 0U;
}

const char *hfoc_build_id(void)
{
    return "testbuild";
}

const char *hfoc_version_string(void)
{
    return "test";
}

typedef void (*command_test_reader_fn)(void *context, const char *text,
                                       size_t length);

uint32_t appliance_boot_log_store_read_records(
    command_test_reader_fn reader, void *context,
    uint32_t max_records)
{
    static const char header[] =
        "BOOTLOG: state=READY records=2 pending=0 dropped=0\n";

    if (reader == NULL) {
        return 0U;
    }
    reader(context, header, sizeof(header) - 1U);
    if (max_records >= 2U) {
        reader(context, "#40 first record\n", 18U);
        reader(context, "#41 second record\n", 19U);
        return 2U;
    }
    if (max_records >= 1U) {
        reader(context, "#41 second record\n", 19U);
        return 1U;
    }
    return 0U;
}

static size_t find_text(const uint8_t *buffer, size_t length,
                        const char *needle)
{
    const size_t needle_length = strlen(needle);
    size_t index;

    if (needle_length == 0U || needle_length > length) {
        return (size_t)-1;
    }
    for (index = 0U; index + needle_length <= length; ++index) {
        if (memcmp(buffer + index, needle, needle_length) == 0) {
            return index;
        }
    }
    return (size_t)-1;
}

static bool text_matches(const uint8_t *buffer, size_t length,
                         const char *needle)
{
    return find_text(buffer, length, needle) != (size_t)-1;
}

static int dispatch_text(const char *command, uint8_t *output,
                         size_t capacity, size_t *length)
{
    int status = 127;

    if (!appliance_ssh_command_dispatch(command, strlen(command), output,
                                        capacity, length, &status)) {
        return -1;
    }
    return status;
}

static void expect_refused(const char *command)
{
    uint8_t output[HFOC_SSH_COMMAND_OUTPUT_MAX];
    size_t length = 0U;
    const int status = dispatch_text(command, output, sizeof(output), &length);

    if (status < 0) {
        return; /* a syntactically invalid command is refused by dispatch */
    }
    CHECK(status != 0, "'%s' was accepted with status 0", command);
    CHECK(text_matches(output, length, "refused"),
          "refusal for '%s' has no refusal body", command);
}

static uint32_t parse_hex32(const uint8_t *text, size_t length, size_t at)
{
    uint32_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U && at + index < length; ++index) {
        const uint8_t byte = text[at + index];
        uint32_t digit;

        if (byte >= '0' && byte <= '9') {
            digit = (uint32_t)(byte - '0');
        } else if (byte >= 'A' && byte <= 'F') {
            digit = (uint32_t)(byte - 'A') + 10U;
        } else if (byte >= 'a' && byte <= 'f') {
            digit = (uint32_t)(byte - 'a') + 10U;
        } else {
            return 0xFFFFFFFFU;
        }
        value = (value << 4) | digit;
    }
    return value;
}

static void test_commands(void)
{
    uint8_t output[HFOC_SSH_COMMAND_OUTPUT_MAX];
    size_t length = 0U;
    int status;

    status = dispatch_text("hfoc status", output, sizeof(output), &length);
    CHECK(status == 0, "hfoc status status=%d", status);
    CHECK(text_matches(output, length, "[NET-STATUS]"), "status lost network");
    CHECK(text_matches(output, length, "[RNG]"), "status lost entropy");

    status = dispatch_text("hfoc network", output, sizeof(output), &length);
    CHECK(status == 0, "hfoc network status=%d", status);
    CHECK(text_matches(output, length, "[NET-STATUS]"), "network lost driver");
    CHECK(text_matches(output, length, "[PANEL]"), "network lost panel");
    CHECK(text_matches(output, length, "[PRODUCTION-SSH]"),
          "network lost SSH readiness");
    CHECK(!text_matches(output, length, "[RNG]"),
          "network printed an unrelated owner record");

    status = dispatch_text("hfoc journal", output, sizeof(output), &length);
    CHECK(status == 0, "hfoc journal status=%d", status);
    CHECK(text_matches(output, length, "BOOTLOG: state=READY"),
          "journal lost the log state");
    CHECK(text_matches(output, length, "#41 second record"),
          "journal lost the newest record");

    status = dispatch_text("hfoc diag", output, sizeof(output), &length);
    CHECK(status == 0, "hfoc diag status=%d", status);
    CHECK(text_matches(output, length, "[DIAG] id=0000002A state=BEGIN"),
          "diag lost the begin marker");
    CHECK(text_matches(output, length, "state=END records=3 crc="),
          "diag lost the end marker/count");
    CHECK(text_matches(output, length, "[DIAG] transport=ssh mode=STREAM"),
          "diag did not name the stream mode");
    CHECK(!text_matches(output, length, "SAVED"),
          "diag claimed a medium save");
    {
        const size_t begin = find_text(output, length, "state=BEGIN");
        const size_t end = find_text(output, length, "[DIAG] id=0000002A state=END");
        const size_t crc_at = find_text(output, length, "crc=");
        uint32_t crc = 0xFFFFFFFFU;
        uint32_t records = 0U;
        uint32_t reported;

        CHECK(begin != (size_t)-1 && end != (size_t)-1 &&
                  crc_at != (size_t)-1,
              "diag framing is incomplete");
        if (begin == (size_t)-1 || end == (size_t)-1 ||
            crc_at == (size_t)-1) {
            return;
        }
        reported = parse_hex32(output, length, crc_at + 4U);
        {
            size_t first_newline = begin;
            size_t line_start;
            size_t line_end;

            while (first_newline < length && output[first_newline] != '\n') {
                ++first_newline;
            }
            line_start = first_newline + 1U;
            while (line_start < end) {
                line_end = line_start;
                while (line_end < length && output[line_end] != '\n') {
                    ++line_end;
                }
                if (line_end == line_start || line_end >= end) {
                    break;
                }
                crc = diag_export_crc(crc, (const char *)output + line_start,
                                      line_end - line_start);
                ++records;
                line_start = line_end + 1U;
            }
        }
        crc = ~crc;
        CHECK(records == 3U, "diag carried %u records, want 3", records);
        CHECK(reported != 0xFFFFFFFFU && reported == crc,
              "diag CRC=%08X does not match the records (%08X)", reported,
              crc);
    }

    expect_refused("hfoc statu");
    expect_refused("hfoc status ");
    expect_refused("hfoc status;id");
    expect_refused("hfoc diag;");
    expect_refused("hfoc network extra");
    expect_refused("echo pwned");
    expect_refused("hfoc");
}

static void test_invalid_bytes(void)
{
    uint8_t output[HFOC_SSH_COMMAND_OUTPUT_MAX];
    size_t length = 0U;
    int status = 0;
    static const uint8_t newline[] = "hfoc diag\n";
    static const uint8_t nul[] = {'h', 'f', 'o', 'c', ' ', 'd', 'i', 'a',
                                  'g', 0, 'x'};
    static const uint8_t utf8[] = {'h', 'f', 'o', 'c', ' ', 0xC3, 0xA9, 'g'};

    CHECK(!appliance_ssh_command_dispatch(
              (const char *)newline, sizeof(newline) - 1U, output,
              sizeof(output), &length, &status),
          "a newline command was parsed");
    CHECK(!appliance_ssh_command_dispatch(
              (const char *)nul, sizeof(nul), output, sizeof(output),
              &length, &status),
          "a NUL command was parsed");
    CHECK(!appliance_ssh_command_dispatch(
              (const char *)utf8, sizeof(utf8), output, sizeof(output),
              &length, &status),
          "a non-ASCII command was parsed");
    CHECK(!appliance_ssh_command_dispatch(
              "", 0U, output, sizeof(output), &length, &status),
          "an empty command was parsed");
}

static void test_overflow(void)
{
    uint8_t output[HFOC_SSH_COMMAND_OUTPUT_MAX];
    size_t length = 0U;
    int status;

    diag_extra_records = 200U;
    status = dispatch_text("hfoc diag", output, sizeof(output), &length);
    CHECK(status != 0, "an oversized diagnostic was published as success");
    CHECK(text_matches(output, length, "exceeds"),
          "the oversized diagnostic has no bound refusal");
    diag_extra_records = 0U;
}

int main(void)
{
    test_commands();
    test_invalid_bytes();
    test_overflow();
    if (failures != 0) {
        fprintf(stderr, "appliance ssh command host: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance ssh command host: ALL OK\n");
    return 0;
}
