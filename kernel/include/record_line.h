#ifndef HFOC_RECORD_LINE_H
#define HFOC_RECORD_LINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-418: one bounded record line, assembled once and published once.
 *
 * `platform_reset` grew this builder for the records a machine's reboot is
 * reported with; the power module reports a shutdown the same way, and a second
 * copy of the same rendering rules would be a second place for them to drift.
 * The builder is pure formatting - it touches no device and no console - so a
 * host proof can build a record and read it back character by character, and
 * the module that owns the record publishes it (serial and the framebuffer
 * mirror, byte for byte the same text).
 *
 * A line that would overflow its bound is marked, never truncated: a record an
 * operator reads must be the record the module meant to write.
 */
#define RECORD_LINE_MAX 200U

struct record_line {
    char text[RECORD_LINE_MAX];
    size_t length;
    bool overflow;
};

void record_line_begin(struct record_line *line);
void record_line_append(struct record_line *line, const char *text);
void record_line_u64(struct record_line *line, uint64_t value);
void record_line_hex(struct record_line *line, uint64_t value);

#endif
