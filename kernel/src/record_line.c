#include "record_line.h"

#include <string.h>

void record_line_begin(struct record_line *line) {
    if (line == NULL) {
        return;
    }
    line->length = 0U;
    line->overflow = false;
    line->text[0] = '\0';
}

void record_line_append(struct record_line *line, const char *text) {
    size_t length = 0U;

    if (line == NULL || line->overflow || text == NULL) {
        if (line != NULL) {
            line->overflow = true;
        }
        return;
    }
    while (text[length] != '\0') {
        ++length;
    }
    if (line->length + length >= sizeof(line->text)) {
        line->overflow = true;
        return;
    }
    memcpy(line->text + line->length, text, length);
    line->length += length;
    line->text[line->length] = '\0';
}

void record_line_u64(struct record_line *line, uint64_t value) {
    char digits[20U];
    size_t count = 0U;

    if (line == NULL || line->overflow) {
        return;
    }
    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    if (line->length + count >= sizeof(line->text)) {
        line->overflow = true;
        return;
    }
    while (count > 0U) {
        line->text[line->length++] = digits[--count];
    }
    line->text[line->length] = '\0';
}

void record_line_hex(struct record_line *line, uint64_t value) {
    static const char digits[16] = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
    };
    char rendered[16U];
    size_t count = 0U;

    if (line == NULL || line->overflow) {
        return;
    }
    do {
        rendered[count++] = digits[value & 0xFU];
        value >>= 4U;
    } while (value != 0U);
    if (line->length + count + 2U >= sizeof(line->text)) {
        line->overflow = true;
        return;
    }
    line->text[line->length++] = '0';
    line->text[line->length++] = 'x';
    while (count > 0U) {
        line->text[line->length++] = rendered[--count];
    }
    line->text[line->length] = '\0';
}
