#ifndef HFOC_USER_COPY_H
#define HFOC_USER_COPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-624: the only path from a user pointer into kernel memory.
 *
 * A caller never dereferences a user address directly.  The complete range is
 * validated against the current address space before the first byte is copied,
 * so a fault or a supervisor-only page cannot leave a partial kernel buffer.
 * Lengths are explicit and bounded by the caller's own structure; this unit
 * refuses zero, overflow and the huge copy that would only be a denial of
 * service.
 */
enum user_copy_status {
    USER_COPY_OK = 0,
    USER_COPY_NULL = 1,
    USER_COPY_RANGE = 2,
    USER_COPY_NOT_MAPPED = 3,
    USER_COPY_BAD_ACCESS = 4,
    USER_COPY_NO_NUL = 5,
};

enum user_copy_status user_copy_check(uint64_t address, size_t length,
                                      bool write);
enum user_copy_status user_copy_in(void *destination, uint64_t user_source,
                                   size_t length);
enum user_copy_status user_copy_out(uint64_t user_destination,
                                    const void *source, size_t length);
/*
 * Copy a NUL-terminated user string into `destination` (capacity includes the
 * NUL).  The whole `capacity` window is validated before it is read, so a
 * string without a terminator is refused instead of scanning past the buffer.
 */
enum user_copy_status user_copy_string_in(char *destination, size_t capacity,
                                          uint64_t user_source,
                                          size_t *length_out);
const char *user_copy_status_string(enum user_copy_status status);

#endif
