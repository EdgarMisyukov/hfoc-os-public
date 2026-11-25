#ifndef HFOC_KERNEL_STATUS_FORMAT_H
#define HFOC_KERNEL_STATUS_FORMAT_H

#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-583: the bounded status-record formatters.
 *
 * The `[NET-STATUS]` record used `kernel_status_append_hex16()` to publish a
 * PCI vendor/device id.  Its loop started at 4 and decremented an unsigned
 * index, so it emitted five digits and shifted the id (`0x10EC0` instead of
 * `0x10EC`).  The formatter now lives here, where a host proof can read the
 * exact bytes the record carries.
 */
size_t kernel_status_append_hex16(char *buffer, size_t capacity,
                                  size_t length, uint16_t value);

#endif
