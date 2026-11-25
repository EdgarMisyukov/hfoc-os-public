#include "appliance_boot_log.h"

#include <stddef.h>
#include <string.h>

/*
 * ITEM-410: the pure half of the appliance boot log.  Everything here is
 * arithmetic over byte arrays: the on-disk slot format, its CRC, the bounded
 * staging ring and the flush policy.  Nothing here touches a device, the clock
 * or the console, so the host proof exercises exactly the bytes the appliance
 * will write and the bytes a reader will have to understand.
 */

#define APPLIANCE_BOOT_LOG_HEADER_MAGIC "HFOC-BOOTLOG"
#define APPLIANCE_BOOT_LOG_HEADER_VERSION_FIELD 1U
#define APPLIANCE_BOOT_LOG_HEADER_GENERATION_WIDTH 10U
#define APPLIANCE_BOOT_LOG_HEADER_SEQUENCE_WIDTH 20U
#define APPLIANCE_BOOT_LOG_HEADER_CURSOR_WIDTH 10U
#define APPLIANCE_BOOT_LOG_HEADER_DROPPED_WIDTH 10U
#define APPLIANCE_BOOT_LOG_HEADER_WRITTEN_WIDTH 20U
#define APPLIANCE_BOOT_LOG_HEADER_CRC_DIGITS 8U
#define APPLIANCE_BOOT_LOG_RECORD_SEQUENCE_WIDTH 20U
#define APPLIANCE_BOOT_LOG_RECORD_GENERATION_WIDTH 10U
#define APPLIANCE_BOOT_LOG_RECORD_TICKS_WIDTH 20U
#define APPLIANCE_BOOT_LOG_RECORD_LENGTH_WIDTH 3U
#define APPLIANCE_BOOT_LOG_RECORD_CRC_DIGITS 8U
/* The decimal value a ten-digit field can carry; a bigger one cannot be
 * rendered in the fixed width and is refused instead of corrupting the line. */
#define APPLIANCE_BOOT_LOG_FIELD10_MAX UINT64_C(9999999999)
#define APPLIANCE_BOOT_LOG_DECIMAL_MAX UINT64_C(18446744073709551615)

static const char appliance_boot_log_hex_digits[] = "0123456789ABCDEF";

uint32_t appliance_boot_log_crc32(const unsigned char *bytes, size_t length)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    size_t index;

    if (bytes == NULL) {
        return 0U;
    }
    for (index = 0U; index < length; ++index) {
        uint32_t bit;

        crc ^= (uint32_t)bytes[index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U
                      ? (crc >> 1) ^ UINT32_C(0xEDB88320)
                      : crc >> 1;
        }
    }
    return ~crc;
}

uint32_t appliance_boot_log_crc32_string(const char *text)
{
    if (text == NULL) {
        return 0U;
    }
    return appliance_boot_log_crc32((const unsigned char *)text,
                                    strlen(text));
}

static uint32_t appliance_boot_log_crc32_update(
    uint32_t crc,
    const unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        uint32_t bit;

        crc ^= (uint32_t)bytes[index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U
                      ? (crc >> 1) ^ UINT32_C(0xEDB88320)
                      : crc >> 1;
        }
    }
    return crc;
}

static void appliance_boot_log_slot_clear(
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE])
{
    memset(slot, 0, APPLIANCE_BOOT_LOG_SLOT_SIZE);
}

static bool appliance_boot_log_slot_zero(
    const unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE])
{
    size_t index;

    for (index = 0U; index < APPLIANCE_BOOT_LOG_SLOT_SIZE; ++index) {
        if (slot[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool appliance_boot_log_text_valid(const char *text, size_t length)
{
    size_t index;

    if (text == NULL || length > APPLIANCE_BOOT_LOG_TEXT_MAX) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)text[index];

        if (value < 0x20U || value > 0x7EU) {
            return false;
        }
    }
    return true;
}

static bool appliance_boot_log_append_literal(
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE],
    size_t *offset,
    const char *literal)
{
    size_t index = 0U;

    if (slot == NULL || offset == NULL || literal == NULL) {
        return false;
    }
    while (literal[index] != '\0') {
        if (*offset + index >= APPLIANCE_BOOT_LOG_SLOT_SIZE) {
            return false;
        }
        slot[*offset + index] = (unsigned char)literal[index];
        ++index;
    }
    *offset += index;
    return true;
}

/* Fixed-width zero-padded decimal, exactly what the reader parses back. */
static bool appliance_boot_log_append_decimal(
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE],
    size_t *offset,
    uint64_t value,
    size_t width)
{
    size_t index;

    if (slot == NULL || offset == NULL || width == 0U ||
        *offset + width > APPLIANCE_BOOT_LOG_SLOT_SIZE) {
        return false;
    }
    for (index = 0U; index < width; ++index) {
        const size_t position = *offset + (width - 1U - index);

        slot[position] = (unsigned char)('0' + (value % 10U));
        value /= 10U;
    }
    if (value != 0U) {
        /* The value does not fit the declared field width. */
        return false;
    }
    *offset += width;
    return true;
}

static void appliance_boot_log_append_hex32(
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE],
    size_t offset,
    uint32_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        const unsigned int shift = (unsigned int)((7U - index) * 4U);

        slot[offset + index] =
            (unsigned char)appliance_boot_log_hex_digits[
                (value >> shift) & 0x0FU];
    }
}

static bool appliance_boot_log_expect_literal(
    const unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE],
    size_t *offset,
    const char *literal)
{
    size_t index = 0U;

    while (literal[index] != '\0') {
        if (*offset + index >= APPLIANCE_BOOT_LOG_SLOT_SIZE ||
            slot[*offset + index] != (unsigned char)literal[index]) {
            return false;
        }
        ++index;
    }
    *offset += index;
    return true;
}

static bool appliance_boot_log_read_decimal(
    const unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE],
    size_t *offset,
    size_t width,
    uint64_t *value_out)
{
    uint64_t value = 0U;
    size_t index;

    if (offset == NULL || value_out == NULL || width == 0U) {
        return false;
    }
    for (index = 0U; index < width; ++index) {
        const unsigned char byte = slot[*offset + index];
        uint64_t digit;

        if (byte < (unsigned char)'0' || byte > (unsigned char)'9') {
            return false;
        }
        digit = (uint64_t)(byte - (unsigned char)'0');
        if (value > (APPLIANCE_BOOT_LOG_DECIMAL_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    *offset += width;
    *value_out = value;
    return true;
}

/*
 * The header line, rendered and verified with the same field widths.  The CRC
 * covers everything before the CRC digits, so a reader can recompute it from
 * the bytes it just parsed.
 */
enum appliance_boot_log_status appliance_boot_log_render_header(
    const struct appliance_boot_log_header *header,
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE])
{
    size_t offset = 0U;
    uint32_t crc;

    if (header == NULL || slot == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    if (header->version != APPLIANCE_BOOT_LOG_VERSION ||
        header->slot_size != APPLIANCE_BOOT_LOG_SLOT_SIZE ||
        header->slot_count != APPLIANCE_BOOT_LOG_SLOT_COUNT ||
        header->record_capacity != APPLIANCE_BOOT_LOG_RECORD_CAPACITY ||
        header->generation > APPLIANCE_BOOT_LOG_FIELD10_MAX ||
        header->cursor >= APPLIANCE_BOOT_LOG_RECORD_CAPACITY) {
        return APPLIANCE_BOOT_LOG_BAD_SLOT;
    }
    appliance_boot_log_slot_clear(slot);
    if (!appliance_boot_log_append_literal(
            slot, &offset, APPLIANCE_BOOT_LOG_HEADER_MAGIC) ||
        !appliance_boot_log_append_literal(slot, &offset, " v=") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, (uint64_t)header->version, 1U) ||
        !appliance_boot_log_append_literal(slot, &offset, " slot=") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, (uint64_t)header->slot_size, 3U) ||
        !appliance_boot_log_append_literal(slot, &offset, " slots=") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, (uint64_t)header->slot_count, 3U) ||
        !appliance_boot_log_append_literal(slot, &offset, " records=") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, (uint64_t)header->record_capacity, 3U) ||
        !appliance_boot_log_append_literal(slot, &offset, " gen=") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, header->generation,
            APPLIANCE_BOOT_LOG_HEADER_GENERATION_WIDTH) ||
        !appliance_boot_log_append_literal(slot, &offset, " next=") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, header->next_sequence,
            APPLIANCE_BOOT_LOG_HEADER_SEQUENCE_WIDTH) ||
        !appliance_boot_log_append_literal(slot, &offset, " cursor=") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, header->cursor,
            APPLIANCE_BOOT_LOG_HEADER_CURSOR_WIDTH) ||
        !appliance_boot_log_append_literal(slot, &offset, " dropped=") ||
        !appliance_boot_log_append_decimal(
            slot, &offset,
            /* ten digits: the counter saturates in the field, never the line */
            header->dropped > APPLIANCE_BOOT_LOG_FIELD10_MAX
                ? APPLIANCE_BOOT_LOG_FIELD10_MAX
                : header->dropped,
            APPLIANCE_BOOT_LOG_HEADER_DROPPED_WIDTH) ||
        !appliance_boot_log_append_literal(slot, &offset, " written=") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, header->written,
            APPLIANCE_BOOT_LOG_HEADER_WRITTEN_WIDTH) ||
        !appliance_boot_log_append_literal(slot, &offset, " crc=")) {
        return APPLIANCE_BOOT_LOG_BAD_SLOT;
    }
    crc = appliance_boot_log_crc32(slot, offset);
    appliance_boot_log_append_hex32(slot, offset, crc);
    offset += APPLIANCE_BOOT_LOG_HEADER_CRC_DIGITS;
    if (offset + 1U >= APPLIANCE_BOOT_LOG_SLOT_SIZE) {
        return APPLIANCE_BOOT_LOG_BAD_SLOT;
    }
    slot[offset++] = (unsigned char)'\n';
    return APPLIANCE_BOOT_LOG_OK;
}

enum appliance_boot_log_status appliance_boot_log_parse_header(
    const unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE],
    struct appliance_boot_log_header *header_out)
{
    struct appliance_boot_log_header header;
    size_t offset = 0U;
    size_t crc_offset;
    uint32_t stored_crc = 0U;
    size_t index;

    if (slot == NULL || header_out == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    memset(&header, 0, sizeof(header));
    memset(header_out, 0, sizeof(*header_out));
    if (appliance_boot_log_slot_zero(slot)) {
        return APPLIANCE_BOOT_LOG_EMPTY;
    }
    if (!appliance_boot_log_expect_literal(
            slot, &offset, APPLIANCE_BOOT_LOG_HEADER_MAGIC) ||
        !appliance_boot_log_expect_literal(slot, &offset, " v=") ||
        !appliance_boot_log_read_decimal(slot, &offset, 1U,
                                         &header.version)) {
        return APPLIANCE_BOOT_LOG_BAD_SLOT;
    }
    if (header.version != APPLIANCE_BOOT_LOG_VERSION) {
        return APPLIANCE_BOOT_LOG_BAD_VERSION;
    }
    if (!appliance_boot_log_expect_literal(slot, &offset, " slot=") ||
        !appliance_boot_log_read_decimal(slot, &offset, 3U,
                                         &header.slot_size) ||
        !appliance_boot_log_expect_literal(slot, &offset, " slots=") ||
        !appliance_boot_log_read_decimal(slot, &offset, 3U,
                                         &header.slot_count) ||
        !appliance_boot_log_expect_literal(slot, &offset, " records=") ||
        !appliance_boot_log_read_decimal(
            slot, &offset, 3U, &header.record_capacity) ||
        !appliance_boot_log_expect_literal(slot, &offset, " gen=") ||
        !appliance_boot_log_read_decimal(
            slot, &offset, APPLIANCE_BOOT_LOG_HEADER_GENERATION_WIDTH,
            &header.generation) ||
        !appliance_boot_log_expect_literal(slot, &offset, " next=") ||
        !appliance_boot_log_read_decimal(
            slot, &offset, APPLIANCE_BOOT_LOG_HEADER_SEQUENCE_WIDTH,
            &header.next_sequence) ||
        !appliance_boot_log_expect_literal(slot, &offset, " cursor=") ||
        !appliance_boot_log_read_decimal(
            slot, &offset, APPLIANCE_BOOT_LOG_HEADER_CURSOR_WIDTH,
            &header.cursor) ||
        !appliance_boot_log_expect_literal(slot, &offset, " dropped=") ||
        !appliance_boot_log_read_decimal(
            slot, &offset, APPLIANCE_BOOT_LOG_HEADER_DROPPED_WIDTH,
            &header.dropped) ||
        !appliance_boot_log_expect_literal(slot, &offset, " written=") ||
        !appliance_boot_log_read_decimal(
            slot, &offset, APPLIANCE_BOOT_LOG_HEADER_WRITTEN_WIDTH,
            &header.written) ||
        !appliance_boot_log_expect_literal(slot, &offset, " crc=")) {
        return APPLIANCE_BOOT_LOG_BAD_SLOT;
    }
    crc_offset = offset;
    for (index = 0U; index < APPLIANCE_BOOT_LOG_HEADER_CRC_DIGITS;
         ++index) {
        const unsigned char byte = slot[crc_offset + index];

        stored_crc <<= 4U;
        if (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') {
            stored_crc |= (uint32_t)(byte - (unsigned char)'0');
        } else if (byte >= (unsigned char)'A' &&
                   byte <= (unsigned char)'F') {
            stored_crc |= (uint32_t)(byte - (unsigned char)'A') + 10U;
        } else if (byte >= (unsigned char)'a' &&
                   byte <= (unsigned char)'f') {
            stored_crc |= (uint32_t)(byte - (unsigned char)'a') + 10U;
        } else {
            return APPLIANCE_BOOT_LOG_BAD_SLOT;
        }
    }
    if (appliance_boot_log_crc32(slot, crc_offset) != stored_crc) {
        return APPLIANCE_BOOT_LOG_BAD_SLOT;
    }
    offset += APPLIANCE_BOOT_LOG_HEADER_CRC_DIGITS;
    if (slot[offset] != (unsigned char)'\n') {
        return APPLIANCE_BOOT_LOG_BAD_SLOT;
    }
    ++offset;
    while (offset < APPLIANCE_BOOT_LOG_SLOT_SIZE) {
        if (slot[offset] != 0U) {
            return APPLIANCE_BOOT_LOG_BAD_SLOT;
        }
        ++offset;
    }
    if (header.slot_size != APPLIANCE_BOOT_LOG_SLOT_SIZE ||
        header.slot_count != APPLIANCE_BOOT_LOG_SLOT_COUNT ||
        header.record_capacity != APPLIANCE_BOOT_LOG_RECORD_CAPACITY ||
        header.cursor >= APPLIANCE_BOOT_LOG_RECORD_CAPACITY) {
        return APPLIANCE_BOOT_LOG_BAD_SLOT;
    }
    *header_out = header;
    return APPLIANCE_BOOT_LOG_OK;
}

enum appliance_boot_log_status appliance_boot_log_render_record(
    const struct appliance_boot_log_entry *entry,
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE])
{
    size_t offset = 0U;
    size_t crc_offset;
    uint32_t crc;
    size_t index;

    if (entry == NULL || slot == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    if (!appliance_boot_log_text_valid(entry->text, entry->length)) {
        return APPLIANCE_BOOT_LOG_BAD_TEXT;
    }
    if (entry->generation > APPLIANCE_BOOT_LOG_FIELD10_MAX) {
        return APPLIANCE_BOOT_LOG_BAD_RANGE;
    }
    appliance_boot_log_slot_clear(slot);
    if (!appliance_boot_log_append_literal(slot, &offset, "@") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, entry->sequence,
            APPLIANCE_BOOT_LOG_RECORD_SEQUENCE_WIDTH) ||
        !appliance_boot_log_append_literal(slot, &offset, " g") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, entry->generation,
            APPLIANCE_BOOT_LOG_RECORD_GENERATION_WIDTH) ||
        !appliance_boot_log_append_literal(slot, &offset, " t") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, entry->ticks,
            APPLIANCE_BOOT_LOG_RECORD_TICKS_WIDTH) ||
        !appliance_boot_log_append_literal(slot, &offset, " l") ||
        !appliance_boot_log_append_decimal(
            slot, &offset, (uint64_t)entry->length,
            APPLIANCE_BOOT_LOG_RECORD_LENGTH_WIDTH) ||
        !appliance_boot_log_append_literal(slot, &offset, " c")) {
        return APPLIANCE_BOOT_LOG_BAD_RANGE;
    }
    crc_offset = offset;
    /*
     * The CRC covers the field header *and* the text, so a torn write that
     * lands in the text fails the checksum and is never handed back as text.
     */
    {
        uint32_t running = appliance_boot_log_crc32_update(
            UINT32_C(0xFFFFFFFF), slot, crc_offset);

        running = appliance_boot_log_crc32_update(
            running, (const unsigned char *)entry->text, entry->length);
        crc = ~running;
    }
    appliance_boot_log_append_hex32(slot, crc_offset, crc);
    offset = crc_offset + APPLIANCE_BOOT_LOG_RECORD_CRC_DIGITS;
    if (offset + entry->length + 1U > APPLIANCE_BOOT_LOG_SLOT_SIZE) {
        return APPLIANCE_BOOT_LOG_BAD_TEXT;
    }
    slot[offset++] = (unsigned char)' ';
    for (index = 0U; index < entry->length; ++index) {
        slot[offset++] = (unsigned char)entry->text[index];
    }
    slot[offset++] = (unsigned char)'\n';
    return APPLIANCE_BOOT_LOG_OK;
}

enum appliance_boot_log_slot_status appliance_boot_log_parse_slot(
    const unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE],
    struct appliance_boot_log_entry *entry_out)
{
    struct appliance_boot_log_entry entry;
    size_t offset = 0U;
    size_t crc_offset;
    size_t text_offset;
    uint32_t stored_crc = 0U;
    uint64_t length = 0U;
    uint32_t crc;
    size_t index;

    if (slot == NULL) {
        return APPLIANCE_BOOT_LOG_SLOT_TORN;
    }
    if (entry_out != NULL) {
        memset(entry_out, 0, sizeof(*entry_out));
    }
    if (appliance_boot_log_slot_zero(slot)) {
        return APPLIANCE_BOOT_LOG_SLOT_EMPTY;
    }
    memset(&entry, 0, sizeof(entry));
    if (!appliance_boot_log_expect_literal(slot, &offset, "@") ||
        !appliance_boot_log_read_decimal(
            slot, &offset, APPLIANCE_BOOT_LOG_RECORD_SEQUENCE_WIDTH,
            &entry.sequence) ||
        !appliance_boot_log_expect_literal(slot, &offset, " g") ||
        !appliance_boot_log_read_decimal(
            slot, &offset, APPLIANCE_BOOT_LOG_RECORD_GENERATION_WIDTH,
            &entry.generation) ||
        !appliance_boot_log_expect_literal(slot, &offset, " t") ||
        !appliance_boot_log_read_decimal(
            slot, &offset, APPLIANCE_BOOT_LOG_RECORD_TICKS_WIDTH,
            &entry.ticks) ||
        !appliance_boot_log_expect_literal(slot, &offset, " l") ||
        !appliance_boot_log_read_decimal(
            slot, &offset, APPLIANCE_BOOT_LOG_RECORD_LENGTH_WIDTH,
            &length) ||
        !appliance_boot_log_expect_literal(slot, &offset, " c")) {
        return APPLIANCE_BOOT_LOG_SLOT_TORN;
    }
    crc_offset = offset;
    for (index = 0U; index < APPLIANCE_BOOT_LOG_RECORD_CRC_DIGITS;
         ++index) {
        const unsigned char byte = slot[crc_offset + index];

        stored_crc <<= 4U;
        if (byte >= (unsigned char)'0' && byte <= (unsigned char)'9') {
            stored_crc |= (uint32_t)(byte - (unsigned char)'0');
        } else if (byte >= (unsigned char)'A' &&
                   byte <= (unsigned char)'F') {
            stored_crc |= (uint32_t)(byte - (unsigned char)'A') + 10U;
        } else if (byte >= (unsigned char)'a' &&
                   byte <= (unsigned char)'f') {
            stored_crc |= (uint32_t)(byte - (unsigned char)'a') + 10U;
        } else {
            return APPLIANCE_BOOT_LOG_SLOT_TORN;
        }
    }
    if (length > APPLIANCE_BOOT_LOG_TEXT_MAX) {
        return APPLIANCE_BOOT_LOG_SLOT_TORN;
    }
    text_offset = crc_offset + APPLIANCE_BOOT_LOG_RECORD_CRC_DIGITS;
    if (slot[text_offset] != (unsigned char)' ') {
        return APPLIANCE_BOOT_LOG_SLOT_TORN;
    }
    ++text_offset;
    if (text_offset + (size_t)length >= APPLIANCE_BOOT_LOG_SLOT_SIZE) {
        return APPLIANCE_BOOT_LOG_SLOT_TORN;
    }
    if (slot[text_offset + (size_t)length] != (unsigned char)'\n') {
        return APPLIANCE_BOOT_LOG_SLOT_TORN;
    }
    for (index = 0U; index < (size_t)length; ++index) {
        const unsigned char byte = slot[text_offset + index];

        if (byte < 0x20U || byte > 0x7EU) {
            return APPLIANCE_BOOT_LOG_SLOT_TORN;
        }
        entry.text[index] = (char)byte;
    }
    entry.text[(size_t)length] = '\0';
    entry.length = (size_t)length;
    offset = text_offset + (size_t)length + 1U;
    while (offset < APPLIANCE_BOOT_LOG_SLOT_SIZE) {
        if (slot[offset] != 0U) {
            return APPLIANCE_BOOT_LOG_SLOT_TORN;
        }
        ++offset;
    }
    {
        uint32_t running = appliance_boot_log_crc32_update(
            UINT32_C(0xFFFFFFFF), slot, crc_offset);

        running = appliance_boot_log_crc32_update(
            running, slot + text_offset, (size_t)length);
        crc = ~running;
    }
    if (crc != stored_crc) {
        return APPLIANCE_BOOT_LOG_SLOT_TORN;
    }
    if (entry_out != NULL) {
        *entry_out = entry;
    }
    return APPLIANCE_BOOT_LOG_SLOT_OK;
}

uint32_t appliance_boot_log_slot_for_cursor(uint64_t cursor)
{
    return (uint32_t)(1U + (cursor % APPLIANCE_BOOT_LOG_RECORD_CAPACITY));
}

enum appliance_boot_log_status appliance_boot_log_advance_cursor(
    uint64_t cursor,
    uint64_t *cursor_out,
    uint64_t *generation_out)
{
    if (cursor_out == NULL || generation_out == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    if (cursor >= APPLIANCE_BOOT_LOG_RECORD_CAPACITY) {
        return APPLIANCE_BOOT_LOG_BAD_RANGE;
    }
    *cursor_out = cursor + 1U;
    if (*cursor_out >= APPLIANCE_BOOT_LOG_RECORD_CAPACITY) {
        *cursor_out = 0U;
        *generation_out += 1U;
    }
    return APPLIANCE_BOOT_LOG_OK;
}

enum appliance_boot_log_status appliance_boot_log_ring_init(
    struct appliance_boot_log_ring *ring)
{
    if (ring == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    memset(ring, 0, sizeof(*ring));
    ring->active = true;
    return APPLIANCE_BOOT_LOG_OK;
}

enum appliance_boot_log_status appliance_boot_log_ring_append(
    struct appliance_boot_log_ring *ring,
    const char *text,
    size_t length,
    uint64_t ticks,
    uint64_t *sequence_out)
{
    struct appliance_boot_log_pending_entry *slot;
    uint64_t sequence;

    if (ring == NULL || text == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    if (!ring->active) {
        return APPLIANCE_BOOT_LOG_BAD_STATE;
    }
    if (!appliance_boot_log_text_valid(text, length) || length == 0U) {
        return APPLIANCE_BOOT_LOG_BAD_TEXT;
    }
    /*
     * A ring full of unwritten records loses its oldest one: the newest record
     * is the one that explains how the boot ended, and the loss is counted so
     * the header can publish it.
     */
    if (ring->next_sequence - ring->written_sequence >=
        APPLIANCE_BOOT_LOG_PENDING_CAPACITY) {
        ring->written_sequence += 1U;
        ring->dropped += 1U;
    }
    sequence = ring->next_sequence;
    slot = &ring->entries[
        (size_t)(sequence % APPLIANCE_BOOT_LOG_PENDING_CAPACITY)];
    memset(slot, 0, sizeof(*slot));
    slot->sequence = sequence;
    slot->ticks = ticks;
    slot->length = length;
    memcpy(slot->text, text, length);
    slot->text[length] = '\0';
    ring->next_sequence = sequence + 1U;
    if (sequence_out != NULL) {
        *sequence_out = sequence;
    }
    return APPLIANCE_BOOT_LOG_OK;
}

enum appliance_boot_log_status appliance_boot_log_ring_next(
    struct appliance_boot_log_ring *ring,
    const struct appliance_boot_log_header *header,
    struct appliance_boot_log_pending_entry *entry_out,
    struct appliance_boot_log_write_plan *plan_out)
{
    if (ring == NULL || header == NULL || entry_out == NULL ||
        plan_out == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    if (!ring->active) {
        return APPLIANCE_BOOT_LOG_BAD_STATE;
    }
    while (ring->written_sequence < ring->next_sequence) {
        const uint64_t sequence = ring->written_sequence;
        const struct appliance_boot_log_pending_entry *slot =
            &ring->entries[
                (size_t)(sequence % APPLIANCE_BOOT_LOG_PENDING_CAPACITY)];

        if (slot->sequence != sequence) {
            /* The ring had to drop this one before it was ever written. */
            ring->written_sequence += 1U;
            continue;
        }
        *entry_out = *slot;
        memset(plan_out, 0, sizeof(*plan_out));
        plan_out->sequence = sequence;
        plan_out->generation = header->generation;
        plan_out->slot_index = appliance_boot_log_slot_for_cursor(
            header->cursor);
        return APPLIANCE_BOOT_LOG_OK;
    }
    return APPLIANCE_BOOT_LOG_EMPTY;
}

enum appliance_boot_log_status appliance_boot_log_ring_peek(
    const struct appliance_boot_log_ring *ring,
    const struct appliance_boot_log_header *header,
    uint64_t index,
    struct appliance_boot_log_pending_entry *entry_out,
    struct appliance_boot_log_write_plan *plan_out)
{
    uint64_t sequence;
    uint64_t seen = 0U;

    if (ring == NULL || header == NULL || entry_out == NULL ||
        plan_out == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    if (!ring->active) {
        return APPLIANCE_BOOT_LOG_BAD_STATE;
    }
    for (sequence = ring->written_sequence;
         sequence < ring->next_sequence; ++sequence) {
        const struct appliance_boot_log_pending_entry *slot =
            &ring->entries[
                (size_t)(sequence % APPLIANCE_BOOT_LOG_PENDING_CAPACITY)];

        if (slot->sequence != sequence) {
            /* Dropped before it was ever written: not part of the flush. */
            continue;
        }
        if (seen == index) {
            const uint64_t cursor = header->cursor + seen;

            *entry_out = *slot;
            memset(plan_out, 0, sizeof(*plan_out));
            plan_out->sequence = sequence;
            /*
             * The generation the header will carry once the records before
             * this one are retired: the cursor wraps at the record capacity
             * and every wrap is one generation.
             */
            plan_out->generation =
                header->generation +
                cursor / (uint64_t)APPLIANCE_BOOT_LOG_RECORD_CAPACITY;
            plan_out->slot_index = appliance_boot_log_slot_for_cursor(cursor);
            return APPLIANCE_BOOT_LOG_OK;
        }
        ++seen;
    }
    return APPLIANCE_BOOT_LOG_EMPTY;
}

enum appliance_boot_log_status appliance_boot_log_ring_mark_written(
    struct appliance_boot_log_ring *ring)
{
    if (ring == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    if (!ring->active) {
        return APPLIANCE_BOOT_LOG_BAD_STATE;
    }
    if (ring->written_sequence >= ring->next_sequence) {
        return APPLIANCE_BOOT_LOG_EMPTY;
    }
    ring->written_sequence += 1U;
    return APPLIANCE_BOOT_LOG_OK;
}

/*
 * The rebase buffer is a static, not a stack object: the ring holds the whole
 * pending history, and the one caller (the attach) is not reentrant.
 */
static struct appliance_boot_log_pending_entry
    appliance_boot_log_rebase_buffer[
        APPLIANCE_BOOT_LOG_PENDING_CAPACITY];

enum appliance_boot_log_status appliance_boot_log_ring_rebase(
    struct appliance_boot_log_ring *ring,
    uint64_t first_sequence)
{
    uint64_t count;
    uint64_t first;
    uint64_t index;

    if (ring == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    if (!ring->active) {
        return APPLIANCE_BOOT_LOG_BAD_STATE;
    }
    count = ring->next_sequence - ring->written_sequence;
    first = ring->written_sequence;
    if (count > APPLIANCE_BOOT_LOG_PENDING_CAPACITY) {
        return APPLIANCE_BOOT_LOG_BAD_RANGE;
    }
    if (count == 0U) {
        ring->written_sequence = first_sequence;
        ring->next_sequence = first_sequence;
        return APPLIANCE_BOOT_LOG_OK;
    }
    for (index = 0U; index < count; ++index) {
        const struct appliance_boot_log_pending_entry *slot =
            &ring->entries[(size_t)((first + index) %
                                    APPLIANCE_BOOT_LOG_PENDING_CAPACITY)];

        if (slot->sequence != first + index) {
            /* A record the ring dropped: the pending range is not contiguous
             * and cannot be renumbered without inventing one. */
            return APPLIANCE_BOOT_LOG_BAD_STATE;
        }
        appliance_boot_log_rebase_buffer[index] = *slot;
    }
    for (index = 0U; index < count; ++index) {
        appliance_boot_log_rebase_buffer[index].sequence =
            first_sequence + index;
        ring->entries[(size_t)((first_sequence + index) %
                               APPLIANCE_BOOT_LOG_PENDING_CAPACITY)] =
            appliance_boot_log_rebase_buffer[index];
    }
    ring->written_sequence = first_sequence;
    ring->next_sequence = first_sequence + count;
    return APPLIANCE_BOOT_LOG_OK;
}

enum appliance_boot_log_status appliance_boot_log_ring_entry(
    const struct appliance_boot_log_ring *ring,
    uint64_t sequence,
    struct appliance_boot_log_pending_entry *entry_out)
{
    const struct appliance_boot_log_pending_entry *slot;

    if (ring == NULL || entry_out == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    if (!ring->active) {
        return APPLIANCE_BOOT_LOG_BAD_STATE;
    }
    slot = &ring->entries[
        (size_t)(sequence % APPLIANCE_BOOT_LOG_PENDING_CAPACITY)];
    if (slot->sequence != sequence) {
        return APPLIANCE_BOOT_LOG_EMPTY;
    }
    *entry_out = *slot;
    return APPLIANCE_BOOT_LOG_OK;
}

enum appliance_boot_log_status appliance_boot_log_ring_state(
    const struct appliance_boot_log_ring *ring,
    uint32_t *pending_out,
    uint64_t *dropped_out)
{
    if (ring == NULL) {
        return APPLIANCE_BOOT_LOG_NULL;
    }
    if (!ring->active) {
        return APPLIANCE_BOOT_LOG_BAD_STATE;
    }
    if (pending_out != NULL) {
        *pending_out =
            (uint32_t)(ring->next_sequence - ring->written_sequence);
    }
    if (dropped_out != NULL) {
        *dropped_out = ring->dropped;
    }
    return APPLIANCE_BOOT_LOG_OK;
}

static bool appliance_boot_log_contains(
    const char *text,
    size_t length,
    const char *needle)
{
    size_t needle_length = 0U;
    size_t index;

    if (text == NULL || needle == NULL) {
        return false;
    }
    while (needle[needle_length] != '\0') {
        ++needle_length;
    }
    if (needle_length == 0U || length < needle_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= length; ++index) {
        if (memcmp(text + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * The moments that matter.  Each entry is a marker this kernel's own records
 * carry, so the policy is a bounded table rather than a judgement about what a
 * record means: the first group names the stages a hardware run quotes (the
 * boot chain, the storage verdict, the install, the management binding, the
 * network mode, DHCP and DNS, the serving path, the TLS lifecycle and the
 * audit chain), the second group names a refusal or a failure in whatever
 * record carries it.
 */
static const char *const appliance_boot_log_flush_markers[] = {
    "[BOOT-CHAIN]",
    "[APPLIANCE-BOOT]",
    /*
     * ITEM-452: every device the xHCI walk reaches names itself on its own
     * `[USB ]` line - the port it sits on, the role it was bound as or refused
     * for, and the reason.  A machine whose pointer is plugged in but not
     * bound has to be readable from the disk the run came up on, so the device
     * records are a flush moment: one line per device, not per loop turn.
     */
    "[USB ]",
    /*
     * ITEM-453: the port evidence of every xHCI controller.  The 2026-09-19
     * run published two PORTSC values out of the six ports of the controller
     * the owner's receiver is attached to, and the durable journal could not
     * say whether the other four were read at all.  Every port now names
     * itself on its own `[XHCI-PORT]` line, and every protocol group the
     * controller declares on an `[XHCI-PROTO]` line, so the answer to "which
     * port, and what did it say" is on the disk the machine came up on.
     */
    "[XHCI-PORT]",
    "[XHCI-PROTO]",
    "[PRODUCTION-BOOT]",
    "[PRODUCTION-INSTALL]",
    "[PRODUCTION-MGMT-BIND]",
    /*
     * ITEM-449: the forwarding rule the installed entry declares, and the
     * refusal of one that cannot be honoured.  Both are decisions made once
     * at boot, so a machine that came up without a forward has to be readable
     * from the disk it came up on.
     */
    "[PRODUCTION-FORWARD]",
    "[PRODUCTION-FORWARD-DECLARATION]",
    "[PRODUCTION-NET-MODE]",
    "[PRODUCTION-DHCP]",
    "[PRODUCTION-DNS]",
    "[PRODUCTION-SERVING]",
    "[PRODUCTION-TLS]",
    "[AUDIT-INTEGRITY]",
    /*
     * ITEM-428: the loop's own liveness reaches the disk.  Every other marker
     * here is a state change, so a machine that was up but had nothing new to
     * report wrote nothing at all - which is why the 2026-09-16 boot log ends
     * on a DHCP record and cannot say whether the appliance was still running.
     * The heartbeat is periodic, so it is the one record that keeps the log's
     * own timestamps moving and makes a stopped loop readable as one.
     */
    "[PRODUCTION-LOOP]",
    /*
     * ITEM-451: the first frame of each class the receive path distinguishes.
     * The heartbeat is periodic and its clock is the PIT, so the frames the
     * owner's laptop sent could wait a long time for their first record - and
     * on a machine switched off right after the failed ping they never reached
     * the disk at all.  Each class is published once, on the loop turn it
     * first appears, so the ARP request, the echo and the panel SYN that
     * decided the run are on the disk the run came up on.
     */
    "[PRODUCTION-FRAME]",
    /*
     * ITEM-418: an operator who asks the machine to reboot or to power off has
     * asked for the end of this boot, so the request - and the mechanism the
     * platform was asked to use - is on the disk before the machine goes down.
     * A shutdown that a platform ignores is then readable from the disk it
     * stayed alive with, which is exactly the record that answers "why did it
     * not switch off".
     */
    "[PLATFORM-POWER]",
    /*
     * MOUSE-DIAG (owner task 2026-09-27): the three records that must survive
     * on the disk even though they are published in the middle of a storm.
     *
     * The two hardware runs that ended with a silent pointer lost exactly the
     * evidence that would have separated "the device stopped sending" from
     * "the controller stopped delivering": the pending ring holds 64 records,
     * the loop publishes ~20-24 durable records per second, and the flush
     * happens on a marker - so ~58% of a busy run's records were dropped, and
     * the stall snapshot, the watchdog's own record and the iteration maximum
     * were among them.  Naming them here flushes the pending ring at the
     * moment they are written, which is the only thing that makes a
     * once-per-episode diagnostic reliable.  Nothing else about the journal's
     * production semantics changes.
     */
    "[MOUSE-SILENCE",
    "[MOUSE-EP0",
    "[INPUT-WATCHDOG]",
    "[ITER-MAX]",
    /*
     * ITEM-497: the boot's input verdict.  It is one line per boot (or per
     * state change) and it answers the question a machine whose input failed
     * on one boot raises, so it is written the moment it is known - the
     * `-CARRY` copies of earlier boots are deliberately not markers: they ride
     * the heartbeat's flush instead of paying one write each.
     */
    "[INPUT-PATH] boot=",
    /*
     * The log's own `[BOOTLOG] state=...` record is deliberately *not* here:
     * a flush publishes one, so making it urgent would make every flush
     * trigger the next one and the log would spend its extent on its own
     * bookkeeping.  It reaches the extent at the next real moment.
     */
    "state=UNAVAILABLE",
    "state=REFUSED",
    "state=FAILED",
    "state=ERROR",
    "verdict=REFUSED",
};

#define APPLIANCE_BOOT_LOG_FLUSH_MARKER_COUNT \
    (sizeof(appliance_boot_log_flush_markers) / \
     sizeof(appliance_boot_log_flush_markers[0]))

bool appliance_boot_log_flush_trigger(const char *text, size_t length)
{
    size_t index;

    if (text == NULL || length == 0U) {
        return false;
    }
    for (index = 0U; index < APPLIANCE_BOOT_LOG_FLUSH_MARKER_COUNT;
         ++index) {
        if (appliance_boot_log_contains(
                text, length, appliance_boot_log_flush_markers[index])) {
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------- ITEM-497 ----- */

/*
 * The input verdict's own text helpers.  They are deliberately not the slot
 * renderer above: a verdict body is variable-width text a reader greps, not a
 * fixed-width field of the on-disk slot header.
 */
static bool appliance_boot_log_text_append(
    char *buffer,
    size_t size,
    size_t *offset,
    const char *text)
{
    size_t index = 0U;

    if (buffer == NULL || offset == NULL || text == NULL || size == 0U) {
        return false;
    }
    while (text[index] != '\0') {
        if (*offset + index + 1U >= size) {
            return false;
        }
        buffer[*offset + index] = text[index];
        ++index;
    }
    *offset += index;
    buffer[*offset] = '\0';
    return true;
}

static bool appliance_boot_log_text_append_u64(
    char *buffer,
    size_t size,
    size_t *offset,
    uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    if (buffer == NULL || offset == NULL || size == 0U) {
        return false;
    }
    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        if (*offset + 1U >= size) {
            return false;
        }
        buffer[(*offset)++] = digits[--count];
    }
    buffer[*offset] = '\0';
    return true;
}

/*
 * Worst first.  A role this machine never bound is absent; one bound but not
 * armed outranks one armed but silent, because "never armed" is the earlier
 * failure.  An unknown word ranks with NOT_ARMED: a verdict that cannot be
 * proven armed is not reported as armed.
 */
static unsigned int appliance_boot_log_input_role_rank(const char *role)
{
    if (role == NULL) {
        return 3U;
    }
    if (strcmp(role, "ARMED") == 0) {
        return 0U;
    }
    if (strcmp(role, "NO_REPORTS") == 0) {
        return 1U;
    }
    if (strcmp(role, "ABSENT") == 0) {
        return 3U;
    }
    return 2U;
}

const char *appliance_boot_log_input_verdict(
    const char *keyboard,
    const char *mouse)
{
    const unsigned int keyboard_rank =
        appliance_boot_log_input_role_rank(keyboard);
    const unsigned int mouse_rank =
        appliance_boot_log_input_role_rank(mouse);
    unsigned int worst = 0U;
    bool present = false;

    /* An absent role is not a failure of the machine: the roles that are
     * there decide the verdict, and only a machine with neither is NO_DEVICE. */
    if (keyboard_rank != 3U) {
        present = true;
        worst = keyboard_rank;
    }
    if (mouse_rank != 3U) {
        if (!present || mouse_rank > worst) {
            worst = mouse_rank;
        }
        present = true;
    }
    if (!present) {
        return "NO_DEVICE";
    }
    switch (worst) {
    case 0U:
        return "ARMED";
    case 1U:
        return "NO_REPORTS";
    default:
        return "NOT_ARMED";
    }
}

size_t appliance_boot_log_format_input_body(
    char *buffer,
    size_t size,
    const struct appliance_boot_log_input_state *state)
{
    size_t offset = 0U;

    if (buffer == NULL || state == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (!appliance_boot_log_text_append(buffer, size, &offset, "boot=") ||
        !appliance_boot_log_text_append_u64(buffer, size, &offset,
                                            state->boot) ||
        !appliance_boot_log_text_append(buffer, size, &offset, " verdict=") ||
        !appliance_boot_log_text_append(
            buffer, size, &offset,
            appliance_boot_log_input_verdict(state->keyboard,
                                             state->mouse)) ||
        !appliance_boot_log_text_append(buffer, size, &offset, " keyboard=") ||
        !appliance_boot_log_text_append(
            buffer, size, &offset,
            state->keyboard != NULL ? state->keyboard : "ABSENT") ||
        !appliance_boot_log_text_append(buffer, size, &offset, " mouse=") ||
        !appliance_boot_log_text_append(
            buffer, size, &offset,
            state->mouse != NULL ? state->mouse : "ABSENT") ||
        !appliance_boot_log_text_append(buffer, size, &offset, " arm_ms=") ||
        !appliance_boot_log_text_append_u64(buffer, size, &offset,
                                            state->arm_ms) ||
        !appliance_boot_log_text_append(buffer, size, &offset,
                                        " first_report_ms=") ||
        !appliance_boot_log_text_append_u64(buffer, size, &offset,
                                            state->first_report_ms) ||
        !appliance_boot_log_text_append(buffer, size, &offset, " ports=") ||
        !appliance_boot_log_text_append_u64(buffer, size, &offset,
                                            state->ports) ||
        !appliance_boot_log_text_append(buffer, size, &offset, " reports=") ||
        !appliance_boot_log_text_append_u64(buffer, size, &offset,
                                            state->reports)) {
        buffer[0] = '\0';
        return 0U;
    }
    return offset;
}

/* `boot=<decimal>` at the start of a verdict body, and nothing else. */
static bool appliance_boot_log_input_body_boot(
    const char *body,
    size_t length,
    uint64_t *boot_out)
{
    uint64_t value = 0U;
    size_t index = 5U;
    bool digits = false;

    if (body == NULL || boot_out == NULL || length < 6U ||
        memcmp(body, "boot=", 5U) != 0) {
        return false;
    }
    while (index < length && body[index] >= '0' && body[index] <= '9') {
        const uint64_t digit = (uint64_t)(body[index] - '0');

        if (value > (APPLIANCE_BOOT_LOG_DECIMAL_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
        digits = true;
        ++index;
    }
    if (!digits || (index < length && body[index] != ' ')) {
        return false;
    }
    *boot_out = value;
    return true;
}

size_t appliance_boot_log_scan_input_verdicts(
    const unsigned char *slots,
    size_t slot_count,
    struct appliance_boot_log_input_record *out,
    size_t capacity,
    uint64_t *newest_boot_out)
{
    static const char fresh_marker[] = "[INPUT-PATH] ";
    static const char carry_marker[] = "[INPUT-PATH-CARRY] ";
    uint64_t newest_boot = 0U;
    size_t found = 0U;
    size_t index;

    if (newest_boot_out != NULL) {
        *newest_boot_out = 0U;
    }
    if (slots == NULL || out == NULL || capacity == 0U) {
        return 0U;
    }
    for (index = 0U; index < slot_count; ++index) {
        const unsigned char *slot =
            slots + index * APPLIANCE_BOOT_LOG_SLOT_SIZE;
        struct appliance_boot_log_entry entry;
        const char *body = NULL;
        size_t body_length = 0U;
        uint64_t boot = 0U;
        size_t position;

        if (appliance_boot_log_parse_slot(slot, &entry) !=
            APPLIANCE_BOOT_LOG_SLOT_OK) {
            continue;
        }
        if (entry.length > sizeof(fresh_marker) - 1U &&
            memcmp(entry.text, fresh_marker, sizeof(fresh_marker) - 1U) ==
                0) {
            body = entry.text + (sizeof(fresh_marker) - 1U);
            body_length = entry.length - (sizeof(fresh_marker) - 1U);
        } else if (entry.length > sizeof(carry_marker) - 1U &&
                   memcmp(entry.text, carry_marker,
                          sizeof(carry_marker) - 1U) == 0) {
            body = entry.text + (sizeof(carry_marker) - 1U);
            body_length = entry.length - (sizeof(carry_marker) - 1U);
        } else {
            continue;
        }
        if (body_length > APPLIANCE_BOOT_LOG_INPUT_BODY_MAX ||
            !appliance_boot_log_input_body_boot(body, body_length, &boot)) {
            continue;
        }
        if (boot > newest_boot) {
            newest_boot = boot;
        }
        /* One body per boot: the carried copy of an earlier verdict is the
         * same body, and a reader must not see the same boot twice. */
        position = 0U;
        while (position < found && out[position].boot != boot &&
               out[position].boot > boot) {
            ++position;
        }
        if (position < found && out[position].boot == boot) {
            continue;
        }
        if (found == capacity && position >= capacity) {
            continue;
        }
        if (found < capacity) {
            ++found;
        }
        for (size_t move = found - 1U; move > position; --move) {
            out[move] = out[move - 1U];
        }
        out[position].boot = boot;
        out[position].length = body_length;
        memcpy(out[position].text, body, body_length);
        out[position].text[body_length] = '\0';
    }
    if (newest_boot_out != NULL) {
        *newest_boot_out = newest_boot;
    }
    return found;
}

void appliance_boot_log_gate_init(struct appliance_boot_log_gate *gate)
{
    if (gate == NULL) {
        return;
    }
    memset(gate, 0, sizeof(*gate));
}

bool appliance_boot_log_gate_admit(
    struct appliance_boot_log_gate *gate,
    size_t family,
    uint64_t interval_ticks,
    uint64_t ticks)
{
    if (gate == NULL || family >= APPLIANCE_BOOT_LOG_GATE_FAMILIES) {
        return true;
    }
    if (!gate->seen[family]) {
        gate->seen[family] = true;
        gate->last_ticks[family] = ticks;
        gate->admitted[family] = 1U;
        return true;
    }
    if (ticks - gate->last_ticks[family] >= interval_ticks) {
        gate->last_ticks[family] = ticks;
        gate->admitted[family] += 1U;
        return true;
    }
    gate->suppressed[family] += 1U;
    return false;
}

uint64_t appliance_boot_log_gate_suppressed(
    const struct appliance_boot_log_gate *gate)
{
    uint64_t total = 0U;
    size_t index;

    if (gate == NULL) {
        return 0U;
    }
    for (index = 0U; index < APPLIANCE_BOOT_LOG_GATE_FAMILIES; ++index) {
        total += gate->suppressed[index];
    }
    return total;
}

const char *appliance_boot_log_status_string(
    enum appliance_boot_log_status status)
{
    switch (status) {
    case APPLIANCE_BOOT_LOG_OK:
        return "APPLIANCE_BOOT_LOG_OK";
    case APPLIANCE_BOOT_LOG_NULL:
        return "APPLIANCE_BOOT_LOG_NULL";
    case APPLIANCE_BOOT_LOG_BAD_STATE:
        return "APPLIANCE_BOOT_LOG_BAD_STATE";
    case APPLIANCE_BOOT_LOG_BAD_SLOT:
        return "APPLIANCE_BOOT_LOG_BAD_SLOT";
    case APPLIANCE_BOOT_LOG_BAD_VERSION:
        return "APPLIANCE_BOOT_LOG_BAD_VERSION";
    case APPLIANCE_BOOT_LOG_BAD_TEXT:
        return "APPLIANCE_BOOT_LOG_BAD_TEXT";
    case APPLIANCE_BOOT_LOG_BAD_RANGE:
        return "APPLIANCE_BOOT_LOG_BAD_RANGE";
    case APPLIANCE_BOOT_LOG_EMPTY:
        return "APPLIANCE_BOOT_LOG_EMPTY";
    case APPLIANCE_BOOT_LOG_TRUNCATED:
        return "APPLIANCE_BOOT_LOG_TRUNCATED";
    default:
        return NULL;
    }
}

const char *appliance_boot_log_slot_status_string(
    enum appliance_boot_log_slot_status status)
{
    switch (status) {
    case APPLIANCE_BOOT_LOG_SLOT_EMPTY:
        return "EMPTY";
    case APPLIANCE_BOOT_LOG_SLOT_OK:
        return "OK";
    case APPLIANCE_BOOT_LOG_SLOT_TORN:
        return "TORN";
    default:
        return NULL;
    }
}
