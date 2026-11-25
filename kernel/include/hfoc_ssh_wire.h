#ifndef HFOC_SSH_WIRE_H
#define HFOC_SSH_WIRE_H

/*
 * Bounded SSH-v2 wire reader/writer.  No allocation, no I/O: the same
 * functions serve the host proofs and the production transport.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hfoc_ssh_reader {
    const uint8_t *data;
    size_t length;
    size_t offset;
    bool failed;
};

struct hfoc_ssh_writer {
    uint8_t *data;
    size_t capacity;
    size_t length;
    bool failed;
};

void hfoc_ssh_reader_init(struct hfoc_ssh_reader *reader, const uint8_t *data,
                     size_t length);
bool hfoc_ssh_read_u8(struct hfoc_ssh_reader *reader, uint8_t *value);
bool hfoc_ssh_read_bool(struct hfoc_ssh_reader *reader, bool *value);
bool hfoc_ssh_read_u32(struct hfoc_ssh_reader *reader, uint32_t *value);
bool hfoc_ssh_read_string(struct hfoc_ssh_reader *reader, const uint8_t **data,
                     uint32_t *length);
bool hfoc_ssh_read_mpint(struct hfoc_ssh_reader *reader, const uint8_t **data,
                    uint32_t *length);
bool hfoc_ssh_reader_done(const struct hfoc_ssh_reader *reader);
size_t hfoc_ssh_reader_remaining(const struct hfoc_ssh_reader *reader);

void hfoc_ssh_writer_init(struct hfoc_ssh_writer *writer, uint8_t *data, size_t capacity);
bool hfoc_ssh_write_u8(struct hfoc_ssh_writer *writer, uint8_t value);
bool hfoc_ssh_write_bool(struct hfoc_ssh_writer *writer, bool value);
bool hfoc_ssh_write_u32(struct hfoc_ssh_writer *writer, uint32_t value);
bool hfoc_ssh_write_bytes(struct hfoc_ssh_writer *writer, const void *data,
                     size_t length);
bool hfoc_ssh_write_string(struct hfoc_ssh_writer *writer, const void *data,
                      size_t length);
bool hfoc_ssh_write_cstring(struct hfoc_ssh_writer *writer, const char *text);
/* Two's-complement mpint; `data` is big-endian magnitude. */
bool hfoc_ssh_write_mpint(struct hfoc_ssh_writer *writer, const uint8_t *data,
                     size_t length);
/* Returns true if `list` contains `name` as a comma-separated token. */
bool hfoc_ssh_name_list_has(const uint8_t *list, size_t length, const char *name);
/* Copies the token containing `name` into `out` (NUL-terminated). */
bool hfoc_ssh_name_list_copy(const uint8_t *list, size_t length, const char *name,
                        char *out, size_t capacity);

#endif
