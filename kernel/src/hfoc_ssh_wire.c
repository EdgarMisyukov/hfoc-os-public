#include "hfoc_ssh_wire.h"

#include <string.h>

void hfoc_ssh_reader_init(struct hfoc_ssh_reader *reader, const uint8_t *data,
                     size_t length)
{
    reader->data = data;
    reader->length = length;
    reader->offset = 0U;
    reader->failed = false;
}

bool hfoc_ssh_read_u8(struct hfoc_ssh_reader *reader, uint8_t *value)
{
    if (reader->failed || reader->offset + 1U > reader->length) {
        reader->failed = true;
        return false;
    }
    *value = reader->data[reader->offset++];
    return true;
}

bool hfoc_ssh_read_bool(struct hfoc_ssh_reader *reader, bool *value)
{
    uint8_t raw;

    if (!hfoc_ssh_read_u8(reader, &raw)) {
        return false;
    }
    *value = raw != 0U;
    return true;
}

bool hfoc_ssh_read_u32(struct hfoc_ssh_reader *reader, uint32_t *value)
{
    if (reader->failed || reader->offset + 4U > reader->length) {
        reader->failed = true;
        return false;
    }
    *value = ((uint32_t)reader->data[reader->offset] << 24) |
             ((uint32_t)reader->data[reader->offset + 1U] << 16) |
             ((uint32_t)reader->data[reader->offset + 2U] << 8) |
             (uint32_t)reader->data[reader->offset + 3U];
    reader->offset += 4U;
    return true;
}

static bool hfoc_ssh_read_slice(struct hfoc_ssh_reader *reader, const uint8_t **data,
                           uint32_t *length)
{
    uint32_t size;

    if (!hfoc_ssh_read_u32(reader, &size)) {
        return false;
    }
    if (reader->failed || size > reader->length - reader->offset) {
        reader->failed = true;
        return false;
    }
    *data = reader->data + reader->offset;
    *length = size;
    reader->offset += size;
    return true;
}

bool hfoc_ssh_read_string(struct hfoc_ssh_reader *reader, const uint8_t **data,
                     uint32_t *length)
{
    return hfoc_ssh_read_slice(reader, data, length);
}

bool hfoc_ssh_read_mpint(struct hfoc_ssh_reader *reader, const uint8_t **data,
                    uint32_t *length)
{
    if (!hfoc_ssh_read_slice(reader, data, length)) {
        return false;
    }
    /* A negative or non-minimal mpint is not needed by this profile. */
    if (*length != 0U && (((*data)[0] & 0x80U) != 0U)) {
        reader->failed = true;
        return false;
    }
    return true;
}

bool hfoc_ssh_reader_done(const struct hfoc_ssh_reader *reader)
{
    return !reader->failed && reader->offset == reader->length;
}

size_t hfoc_ssh_reader_remaining(const struct hfoc_ssh_reader *reader)
{
    return reader->failed ? 0U : reader->length - reader->offset;
}

void hfoc_ssh_writer_init(struct hfoc_ssh_writer *writer, uint8_t *data, size_t capacity)
{
    writer->data = data;
    writer->capacity = capacity;
    writer->length = 0U;
    writer->failed = false;
}

bool hfoc_ssh_write_u8(struct hfoc_ssh_writer *writer, uint8_t value)
{
    if (writer->failed || writer->length + 1U > writer->capacity) {
        writer->failed = true;
        return false;
    }
    writer->data[writer->length++] = value;
    return true;
}

bool hfoc_ssh_write_bool(struct hfoc_ssh_writer *writer, bool value)
{
    return hfoc_ssh_write_u8(writer, value ? 1U : 0U);
}

bool hfoc_ssh_write_u32(struct hfoc_ssh_writer *writer, uint32_t value)
{
    if (writer->failed || writer->length + 4U > writer->capacity) {
        writer->failed = true;
        return false;
    }
    writer->data[writer->length++] = (uint8_t)(value >> 24);
    writer->data[writer->length++] = (uint8_t)(value >> 16);
    writer->data[writer->length++] = (uint8_t)(value >> 8);
    writer->data[writer->length++] = (uint8_t)value;
    return true;
}

bool hfoc_ssh_write_bytes(struct hfoc_ssh_writer *writer, const void *data,
                     size_t length)
{
    if (writer->failed || writer->length + length > writer->capacity) {
        writer->failed = true;
        return false;
    }
    if (length != 0U) {
        memcpy(writer->data + writer->length, data, length);
        writer->length += length;
    }
    return true;
}

bool hfoc_ssh_write_string(struct hfoc_ssh_writer *writer, const void *data,
                      size_t length)
{
    if (length > UINT32_MAX || !hfoc_ssh_write_u32(writer, (uint32_t)length)) {
        return false;
    }
    if (length != 0U) {
        if (writer->failed || writer->length + length > writer->capacity) {
            writer->failed = true;
            return false;
        }
        memcpy(writer->data + writer->length, data, length);
        writer->length += length;
    }
    return true;
}

bool hfoc_ssh_write_cstring(struct hfoc_ssh_writer *writer, const char *text)
{
    return hfoc_ssh_write_string(writer, text, strlen(text));
}

bool hfoc_ssh_write_mpint(struct hfoc_ssh_writer *writer, const uint8_t *data,
                     size_t length)
{
    uint8_t zero = 0U;

    /* Trim leading zero bytes but keep one value byte for zero. */
    while (length > 1U && data[0] == 0U) {
        ++data;
        --length;
    }
    if (length == 0U) {
        return hfoc_ssh_write_string(writer, &zero, 0U);
    }
    if ((data[0] & 0x80U) != 0U) {
        if (!hfoc_ssh_write_u32(writer, (uint32_t)(length + 1U))) {
            return false;
        }
        if (!hfoc_ssh_write_u8(writer, 0U)) {
            return false;
        }
        if (writer->length + length > writer->capacity) {
            writer->failed = true;
            return false;
        }
        memcpy(writer->data + writer->length, data, length);
        writer->length += length;
        return true;
    }
    return hfoc_ssh_write_string(writer, data, length);
}

bool hfoc_ssh_name_list_has(const uint8_t *list, size_t length, const char *name)
{
    size_t name_length = strlen(name);
    size_t start = 0U;

    while (start <= length) {
        size_t end = start;
        while (end < length && list[end] != ',') {
            ++end;
        }
        if (end - start == name_length &&
            memcmp(list + start, name, name_length) == 0) {
            return true;
        }
        if (end == length) {
            break;
        }
        start = end + 1U;
    }
    return false;
}

bool hfoc_ssh_name_list_copy(const uint8_t *list, size_t length, const char *name,
                        char *out, size_t capacity)
{
    size_t name_length = strlen(name);
    size_t start = 0U;

    while (start <= length) {
        size_t end = start;
        while (end < length && list[end] != ',') {
            ++end;
        }
        if (end - start == name_length &&
            memcmp(list + start, name, name_length) == 0) {
            if (name_length + 1U > capacity) {
                return false;
            }
            memcpy(out, list + start, name_length);
            out[name_length] = '\0';
            return true;
        }
        if (end == length) {
            break;
        }
        start = end + 1U;
    }
    return false;
}
