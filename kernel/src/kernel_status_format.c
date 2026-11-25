#include "kernel_status_format.h"

size_t kernel_status_append_hex16(char *buffer, size_t capacity,
                                  size_t length, uint16_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    uint32_t digit;

    if (buffer == NULL || capacity == 0U) {
        return 0U;
    }
    if (length >= capacity) {
        length = capacity - 1U;
    }
    if (length + 3U < capacity) {
        buffer[length++] = '0';
        buffer[length++] = 'x';
    }
    for (digit = 4U; digit > 0U; --digit) {
        if (length + 1U >= capacity) {
            break;
        }
        buffer[length++] =
            digits[(value >> ((digit - 1U) * 4U)) & 0x0FU];
    }
    buffer[length] = '\0';
    return length;
}
