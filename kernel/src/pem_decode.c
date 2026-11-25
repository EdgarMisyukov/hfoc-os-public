#include "pem_decode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-503: the bounded PEM decoder the BYO import path uses.
 *
 * The decoder walks the text line by line.  A line is the bytes between two
 * newlines; `\r` before the newline is accepted because files written on other
 * systems carry it.  The block begins at a line that is exactly
 * `-----BEGIN <label>-----` and ends at `-----END <label>-----`.  The base64
 * body is collected in full before it is decoded, because only then can the
 * padding rules be checked against the end of the body instead of guessed per
 * line: padding in the middle of a certificate is a malformed document, not
 * something to skip.
 */

#define PEM_LABEL_MAX 48U
/* 4096 DER bytes are 5464 base64 characters; 8192 is that bound with room. */
#define PEM_BASE64_MAX 8192U

/*
 * ITEM-503: the collected base64 body is file-scope rather than an 8 KiB
 * kernel stack frame; the decoder is single-threaded and a chain bundle
 * decodes one block at a time.  The wrapper clears it on every return path,
 * because a private key passes through it.
 */
static char pem_decode_base64[PEM_BASE64_MAX];

static bool pem_decode_label_valid(const char *label)
{
    size_t index = 0U;

    if (label == NULL || label[0] == '\0') {
        return false;
    }
    while (label[index] != '\0') {
        const char character = label[index];

        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == ' ')) {
            return false;
        }
        ++index;
        if (index >= PEM_LABEL_MAX) {
            return false;
        }
    }
    return true;
}

static size_t pem_decode_marker(
    char *buffer,
    size_t capacity,
    const char *prefix,
    const char *label,
    const char *suffix)
{
    size_t length = 0U;
    size_t index = 0U;

    while (prefix[index] != '\0' && length + 1U < capacity) {
        buffer[length++] = prefix[index++];
    }
    index = 0U;
    while (label[index] != '\0' && length + 1U < capacity) {
        buffer[length++] = label[index++];
    }
    index = 0U;
    while (suffix[index] != '\0' && length + 1U < capacity) {
        buffer[length++] = suffix[index++];
    }
    buffer[length] = '\0';
    return length;
}

static bool pem_decode_line(
    const char *text,
    size_t text_length,
    size_t *cursor,
    const char **line,
    size_t *line_length)
{
    size_t start;
    size_t end;

    if (*cursor >= text_length) {
        return false;
    }
    start = *cursor;
    while (*cursor < text_length && text[*cursor] != '\n') {
        ++*cursor;
    }
    end = *cursor;
    if (*cursor < text_length) {
        ++*cursor;
    }
    if (end > start && text[end - 1U] == '\r') {
        --end;
    }
    *line = &text[start];
    *line_length = end - start;
    return true;
}

static bool pem_decode_line_is(
    const char *line,
    size_t line_length,
    const char *expected,
    size_t expected_length)
{
    return line_length == expected_length &&
           memcmp(line, expected, expected_length) == 0;
}

static int pem_decode_value(char character)
{
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

static enum pem_decode_status pem_decode_body(
    const char *base64,
    size_t length,
    unsigned char *output,
    size_t output_capacity,
    size_t *written)
{
    size_t padding = 0U;
    size_t index;

    if (length == 0U || (length % 4U) != 0U) {
        return PEM_DECODE_BAD_BASE64;
    }
    if (base64[length - 1U] == '=') {
        padding = 1U;
        if (length >= 2U && base64[length - 2U] == '=') {
            padding = 2U;
        }
    }
    for (index = 0U; index < length; ++index) {
        if (base64[index] == '=') {
            /* '=' is legal only in the final one or two positions. */
            if (index < length - padding) {
                return PEM_DECODE_BAD_BASE64;
            }
            continue;
        }
        if (pem_decode_value(base64[index]) < 0) {
            return PEM_DECODE_BAD_BASE64;
        }
    }
    for (index = 0U; index + 4U <= length; index += 4U) {
        const int a = pem_decode_value(base64[index]);
        const int b = pem_decode_value(base64[index + 1U]);
        const int c = pem_decode_value(base64[index + 2U]);
        const int d = pem_decode_value(base64[index + 3U]);
        uint32_t triple;
        size_t produced;

        if (a < 0 || b < 0) {
            return PEM_DECODE_BAD_BASE64;
        }
        if ((c < 0 || d < 0) && index + 4U != length) {
            return PEM_DECODE_BAD_BASE64;
        }
        triple = ((uint32_t)a << 18U) | ((uint32_t)b << 12U) |
                 ((uint32_t)(c < 0 ? 0 : c) << 6U) |
                 (uint32_t)(d < 0 ? 0 : d);
        produced = c < 0 ? 1U : (d < 0 ? 2U : 3U);
        if (*written + produced > output_capacity) {
            return PEM_DECODE_TOO_LARGE;
        }
        output[*written] = (unsigned char)((triple >> 16U) & 0xFFU);
        if (produced > 1U) {
            output[*written + 1U] =
                (unsigned char)((triple >> 8U) & 0xFFU);
        }
        if (produced > 2U) {
            output[*written + 2U] = (unsigned char)(triple & 0xFFU);
        }
        *written += produced;
    }
    return PEM_DECODE_OK;
}

static enum pem_decode_status pem_decode_block_impl(
    const char *text,
    size_t text_length,
    const char *label,
    unsigned char *output,
    size_t output_capacity,
    size_t *output_length)
{
    char begin[PEM_LABEL_MAX + 16U];
    char end[PEM_LABEL_MAX + 16U];
    char *const base64 = pem_decode_base64;
    size_t begin_length;
    size_t end_length;
    size_t base64_length = 0U;
    size_t cursor = 0U;
    size_t written = 0U;
    bool inside = false;
    bool closed = false;
    enum pem_decode_status status;

    if (text == NULL || label == NULL || output_length == NULL ||
        (output == NULL && output_capacity != 0U)) {
        return PEM_DECODE_NULL;
    }
    *output_length = 0U;
    if (!pem_decode_label_valid(label)) {
        return PEM_DECODE_BAD_ARMOUR;
    }
    begin_length = pem_decode_marker(
        begin, sizeof(begin), "-----BEGIN ", label, "-----");
    end_length = pem_decode_marker(
        end, sizeof(end), "-----END ", label, "-----");
    /*
     * ITEM-503 follow-up: a valid label (at most PEM_LABEL_MAX - 1 bytes) makes
     * a marker that exactly fills the buffer, so only an overflow is a refusal;
     * the previous `>=` refused that legal boundary case.
     */
    if (begin_length + 1U > sizeof(begin) ||
        end_length + 1U > sizeof(end)) {
        return PEM_DECODE_BAD_ARMOUR;
    }

    while (cursor < text_length) {
        const char *line = NULL;
        size_t line_length = 0U;

        if (!pem_decode_line(
                text, text_length, &cursor, &line, &line_length)) {
            break;
        }
        if (!inside) {
            if (pem_decode_line_is(line, line_length, begin,
                                   begin_length)) {
                inside = true;
            }
            continue;
        }
        if (pem_decode_line_is(line, line_length, end, end_length)) {
            closed = true;
            break;
        }
        /*
         * A second armour line inside the block is a malformed document, not
         * something to skip: the caller asked for one named block and the file
         * does not contain exactly that.
         */
        if (line_length >= 5U && memcmp(line, "-----", 5U) == 0) {
            return PEM_DECODE_BAD_ARMOUR;
        }
        if (base64_length + line_length > PEM_BASE64_MAX) {
            return PEM_DECODE_TOO_LARGE;
        }
        memcpy(&base64[base64_length], line, line_length);
        base64_length += line_length;
    }
    if (!inside) {
        return PEM_DECODE_NO_BLOCK;
    }
    if (!closed) {
        return PEM_DECODE_BAD_ARMOUR;
    }
    status = pem_decode_body(
        base64, base64_length, output, output_capacity, &written);
    if (status != PEM_DECODE_OK) {
        return status;
    }
    *output_length = written;
    return PEM_DECODE_OK;
}

enum pem_decode_status pem_decode_block(
    const char *text,
    size_t text_length,
    const char *label,
    unsigned char *output,
    size_t output_capacity,
    size_t *output_length)
{
    enum pem_decode_status status = pem_decode_block_impl(
        text, text_length, label, output, output_capacity, output_length);
    volatile char *scratch = pem_decode_base64;
    size_t index;

    for (index = 0U; index < sizeof(pem_decode_base64); ++index) {
        scratch[index] = 0;
    }
    return status;
}

const char *pem_decode_status_string(enum pem_decode_status status)
{
    switch (status) {
    case PEM_DECODE_OK:
        return "PEM_DECODE_OK";
    case PEM_DECODE_NULL:
        return "PEM_DECODE_NULL";
    case PEM_DECODE_NO_BLOCK:
        return "PEM_DECODE_NO_BLOCK";
    case PEM_DECODE_BAD_BASE64:
        return "PEM_DECODE_BAD_BASE64";
    case PEM_DECODE_TOO_LARGE:
        return "PEM_DECODE_TOO_LARGE";
    case PEM_DECODE_BAD_ARMOUR:
    default:
        return "PEM_DECODE_BAD_ARMOUR";
    }
}
