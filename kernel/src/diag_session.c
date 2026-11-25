/*
 * ITEM-552: the pure half of the diagnostic export - the session frame, its
 * CRC and the slot decode.
 *
 * The hardware half (kernel/src/diag_export.c) owns the journal and the
 * diagnostics; everything that decides whether a save is a save lives here,
 * where the host proof drives it: the BEGIN/END framing, the session counter
 * encoding, the CRC over a session's records and the rule that turns a read-back
 * slot into text only when the length and the CRC inside it check out.  The
 * Debian extractor applies the same two rules, so a session the kernel calls
 * verified cannot read back as torn on the other machine, and the reverse.
 */

#include "diag_export.h"

#include <stddef.h>

const char *diag_export_result_string(enum diag_export_result result) {
    switch (result) {
    case DIAG_EXPORT_OK:
        return "OK";
    case DIAG_EXPORT_NO_MEDIUM:
        return "NO_MEDIUM";
    case DIAG_EXPORT_NOT_READY:
        return "NOT_READY";
    case DIAG_EXPORT_FULL:
        return "FULL";
    case DIAG_EXPORT_REWIND:
        return "REWIND";
    case DIAG_EXPORT_WRITE:
        return "WRITE";
    case DIAG_EXPORT_VERIFY:
        return "VERIFY";
    case DIAG_EXPORT_OVERFLOW:
        return "OVERFLOW";
    }
    return "UNKNOWN";
}

/* A bounded decimal writer: the framing lines are short and a line that does
   not fit is refused, never cut. */
struct diag_writer {
    char *buffer;
    size_t size;
    size_t length;
    bool overflow;
};

static void diag_text(struct diag_writer *writer, const char *text) {
    if (text == NULL) {
        writer->overflow = true;
        return;
    }
    while (*text != '\0') {
        if (writer->length + 1U >= writer->size) {
            writer->overflow = true;
            return;
        }
        writer->buffer[writer->length++] = *text++;
    }
    writer->buffer[writer->length] = '\0';
}

static void diag_u32(struct diag_writer *writer, uint32_t value) {
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
        diag_text(writer, one);
    }
}

static void diag_hex32(struct diag_writer *writer, uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char text[9];

    for (uint32_t nibble = 0U; nibble < 8U; ++nibble) {
        text[nibble] = digits[(value >> (28U - nibble * 4U)) & 0x0FU];
    }
    text[8] = '\0';
    diag_text(writer, text);
}

/* `[DIAG] id=XXXXXXXX state=BEGIN build=... version=... recorded=N slots=W/C` */
size_t diag_export_begin_line(char *buffer,
                              size_t size,
                              uint32_t id,
                              const char *build,
                              const char *version,
                              uint32_t written,
                              uint32_t capacity) {
    struct diag_writer writer;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    writer.buffer = buffer;
    writer.size = size;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';
    diag_text(&writer, "[DIAG] id=");
    diag_hex32(&writer, id);
    diag_text(&writer, " state=BEGIN build=");
    diag_text(&writer, build != NULL ? build : "unknown");
    diag_text(&writer, " version=");
    diag_text(&writer, version != NULL ? version : "unknown");
    diag_text(&writer, " slots=");
    diag_u32(&writer, written);
    diag_text(&writer, "/");
    diag_u32(&writer, capacity);
    if (writer.overflow) {
        buffer[0] = '\0';
        return 0U;
    }
    return writer.length;
}

/* `[DIAG] id=XXXXXXXX state=END records=N crc=XXXXXXXX` */
size_t diag_export_end_line(char *buffer,
                            size_t size,
                            uint32_t id,
                            uint32_t records,
                            uint32_t crc) {
    struct diag_writer writer;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    writer.buffer = buffer;
    writer.size = size;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';
    diag_text(&writer, "[DIAG] id=");
    diag_hex32(&writer, id);
    diag_text(&writer, " state=END records=");
    diag_u32(&writer, records);
    diag_text(&writer, " crc=");
    diag_hex32(&writer, crc);
    if (writer.overflow) {
        buffer[0] = '\0';
        return 0U;
    }
    return writer.length;
}

/* The one field reader the markers need: `key=` followed by decimal or the
   `id=`/`crc=` hexadecimal forms.  Returns false when the field is absent. */
static bool diag_field(const char *text, const char *key, const char **value) {
    size_t key_length = 0U;

    if (text == NULL || key == NULL || value == NULL) {
        return false;
    }
    while (key[key_length] != '\0') {
        ++key_length;
    }
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        size_t index = 0U;

        if (cursor != text && cursor[-1] != ' ') {
            continue;
        }
        while (index < key_length && cursor[index] == key[index]) {
            ++index;
        }
        if (index == key_length) {
            *value = cursor + key_length;
            return true;
        }
    }
    return false;
}

static bool diag_parse_dec(const char *text, uint32_t *out) {
    uint32_t value = 0U;

    if (text == NULL || out == NULL || *text < '0' || *text > '9') {
        return false;
    }
    while (*text >= '0' && *text <= '9') {
        value = value * 10U + (uint32_t)(*text - '0');
        ++text;
    }
    if (*text != '\0' && *text != ' ') {
        return false;
    }
    *out = value;
    return true;
}

static bool diag_parse_hex8(const char *text, uint32_t *out) {
    uint32_t value = 0U;

    if (text == NULL || out == NULL) {
        return false;
    }
    for (uint32_t index = 0U; index < 8U; ++index) {
        const char digit = text[index];

        value <<= 4U;
        if (digit >= '0' && digit <= '9') {
            value |= (uint32_t)(digit - '0');
        } else if (digit >= 'A' && digit <= 'F') {
            value |= (uint32_t)(digit - 'A') + 10U;
        } else if (digit >= 'a' && digit <= 'f') {
            value |= (uint32_t)(digit - 'a') + 10U;
        } else {
            return false;
        }
    }
    *out = value;
    return true;
}

bool diag_export_parse_marker(const char *text,
                              bool *is_begin,
                              bool *is_end,
                              uint32_t *id,
                              uint32_t *records,
                              uint32_t *crc) {
    const char *field = NULL;
    uint32_t parsed = 0U;

    if (is_begin != NULL) {
        *is_begin = false;
    }
    if (is_end != NULL) {
        *is_end = false;
    }
    if (text == NULL || text[0] != '[') {
        return false;
    }
    if (!diag_field(text, "id=", &field) || !diag_parse_hex8(field, &parsed)) {
        return false;
    }
    if (id != NULL) {
        *id = parsed;
    }
    if (diag_field(text, "state=BEGIN", &field)) {
        if (is_begin != NULL) {
            *is_begin = true;
        }
        return true;
    }
    if (diag_field(text, "state=END", &field)) {
        if (is_end != NULL) {
            *is_end = true;
        }
        if (records != NULL) {
            const char *value = NULL;

            if (!diag_field(text, "records=", &value) ||
                !diag_parse_dec(value, &parsed)) {
                return false;
            }
            *records = parsed;
        }
        if (crc != NULL) {
            const char *value = NULL;

            if (!diag_field(text, "crc=", &value) ||
                !diag_parse_hex8(value, &parsed)) {
                return false;
            }
            *crc = parsed;
        }
        return true;
    }
    return false;
}

/* The raw CRC-32 update, the same polynomial and bit order the journal core
   and the Debian reader use; the caller owns the initial value (all ones) and
   the final complement. */
static uint32_t diag_crc_bytes(uint32_t crc,
                               const uint8_t *bytes,
                               size_t length) {
    static const uint32_t polynomial = 0xEDB88320U;

    for (size_t index = 0U; index < length; ++index) {
        crc ^= bytes[index];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ polynomial : crc >> 1U;
        }
    }
    return crc;
}

uint32_t diag_export_crc(uint32_t crc, const char *text, size_t length) {
    crc = diag_crc_bytes(crc, (const uint8_t *)text, length);
    /* Every record contributes its text and one newline, so two records whose
       concatenation would be the same text still differ. */
    return diag_crc_bytes(crc, (const uint8_t *)"\n", 1U);
}

/* The record frame the journal core writes: length in 0..3, CRC over the text
   in 4..7, the text at 8 (install_journal_core.c, format_record). */
static uint32_t diag_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

size_t diag_export_slot_text(const uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES],
                             char *buffer,
                             size_t size) {
    uint32_t length;
    uint32_t wanted;
    uint32_t computed;
    bool empty = true;

    if (slot == NULL || buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    for (uint32_t index = 0U; index < INSTALL_JOURNAL_SLOT_BYTES; ++index) {
        if (slot[index] != 0U) {
            empty = false;
            break;
        }
    }
    if (empty) {
        return 0U;
    }
    length = diag_le32(&slot[0]);
    if (length == 0U || length > INSTALL_JOURNAL_TEXT_MAX ||
        length + 8U > INSTALL_JOURNAL_SLOT_BYTES) {
        return 0U;
    }
    wanted = diag_le32(&slot[4]);
    /* The journal core frames the text with its own CRC (the standard CRC-32:
       initial value all ones, final complement), so a slot is read back with
       exactly that rule. */
    computed = ~diag_crc_bytes(0xFFFFFFFFU, &slot[8], length);
    if (computed != wanted) {
        return 0U;
    }
    if ((size_t)length + 1U > size) {
        return 0U;
    }
    for (uint32_t index = 0U; index < length; ++index) {
        buffer[index] = (char)slot[8U + index];
    }
    buffer[length] = '\0';
    return (size_t)length;
}
