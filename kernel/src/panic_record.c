/*
 * ITEM-521 (increment 11): the two records that name a panic in the durable
 * journal.  The format is the one the host witness and slice 210 pin:
 *
 *   [PANIC] <message> vector=<decimal> error=0x<16 hex> rip=0x<16 hex>
 *   [PANIC] detail=<detail>
 *
 * The frame fields are the ones the panic screen and the serial frame line
 * already carry, in the same rendering (`serial_write_hex` writes 0x and
 * sixteen upper-case digits), so a reader of the medium sees what an operator
 * in front of the machine would have seen.
 *
 * The copy into the caller's buffer happens only after the whole record was
 * assembled, so a buffer that is one byte short is refused with an empty
 * answer instead of a truncated record (requirement 5: a loss is named, never
 * printed as a half line).  The one clamp that does happen - a message or a
 * detail longer than its bound - is marked with the same `...` tail the
 * durable log uses for its own over-long records, so it is visible rather than
 * silent.
 */

#include "panic_record.h"

#include <string.h>

static size_t panic_record_append(char *record, size_t length,
                                  const char *text) {
    while (*text != '\0') {
        record[length++] = *text++;
    }
    return length;
}

static size_t panic_record_append_u64(char *record, size_t length,
                                      uint64_t value) {
    char digits[20U];
    size_t count = 0U;

    if (value == 0U) {
        record[length++] = '0';
        return length;
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (uint32_t)(value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        record[length++] = digits[--count];
    }
    return length;
}

static size_t panic_record_append_hex64(char *record, size_t length,
                                        uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    record[length++] = '0';
    record[length++] = 'x';
    for (shift = 60; shift >= 0; shift -= 4) {
        record[length++] = digits[(value >> (uint32_t)shift) & 0x0FU];
    }
    return length;
}

/*
 * Copy `text` into the record, clamping it at `bound` bytes with a visible
 * `...` tail.  `bound` is at least the three marker bytes by construction.
 */
static size_t panic_record_append_clamped(char *record, size_t length,
                                          const char *text, size_t bound) {
    const size_t text_length = strlen(text);

    if (text_length <= bound) {
        memcpy(record + length, text, text_length);
        return length + text_length;
    }
    memcpy(record + length, text, bound - 3U);
    memcpy(record + length + bound - 3U, "...", 3U);
    return length + bound;
}

size_t panic_record_render(char *output, size_t capacity, const char *message,
                           bool has_frame, uint64_t vector,
                           uint64_t error_code, uint64_t rip) {
    char record[PANIC_RECORD_MAX];
    size_t length = 0U;

    if (output == NULL || capacity == 0U) {
        return 0U;
    }
    output[0] = '\0';
    length = panic_record_append(record, length, "[PANIC] ");
    length = panic_record_append_clamped(record, length,
                                         message == NULL ? "Unknown panic"
                                                         : message,
                                         PANIC_RECORD_MESSAGE_MAX);
    if (has_frame) {
        length = panic_record_append(record, length, " vector=");
        length = panic_record_append_u64(record, length, vector);
        length = panic_record_append(record, length, " error=");
        length = panic_record_append_hex64(record, length, error_code);
        length = panic_record_append(record, length, " rip=");
        length = panic_record_append_hex64(record, length, rip);
    }
    if (length + 1U > capacity || length + 1U > sizeof(record)) {
        return 0U;
    }
    record[length] = '\0';
    memcpy(output, record, length + 1U);
    return length;
}

size_t panic_record_detail(char *output, size_t capacity, const char *detail) {
    char record[PANIC_RECORD_DETAIL_LINE_MAX];
    size_t length = 0U;

    if (output == NULL || capacity == 0U) {
        return 0U;
    }
    output[0] = '\0';
    if (detail == NULL || detail[0] == '\0') {
        return 0U;
    }
    length = panic_record_append(record, length, "[PANIC] detail=");
    length = panic_record_append_clamped(record, length, detail,
                                         PANIC_RECORD_DETAIL_MAX);
    if (length + 1U > capacity || length + 1U > sizeof(record)) {
        return 0U;
    }
    record[length] = '\0';
    memcpy(output, record, length + 1U);
    return length;
}
