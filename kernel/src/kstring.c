#include "kstring.h"
#include <stdint.h>
#include <limits.h>

void *memcpy(void *dest, const void *src, size_t count) {
    uint8_t *out = dest;
    const uint8_t *in = src;
    for (size_t index = 0; index < count; ++index) {
        out[index] = in[index];
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    uint8_t *out = dest;
    const uint8_t *in = src;
    if (out < in) {
        return memcpy(dest, src, count);
    }
    for (size_t index = count; index > 0; --index) {
        out[index - 1] = in[index - 1];
    }
    return dest;
}

void *memset(void *dest, int value, size_t count) {
    uint8_t *out = dest;
    for (size_t index = 0; index < count; ++index) {
        out[index] = (uint8_t)value;
    }
    return dest;
}

int memcmp(const void *left, const void *right, size_t count) {
    const uint8_t *a = left;
    const uint8_t *b = right;
    for (size_t index = 0; index < count; ++index) {
        if (a[index] != b[index]) {
            return (int)a[index] - (int)b[index];
        }
    }
    return 0;
}

size_t strlen(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

int strcmp(const char *left, const char *right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (a != b || a == '\0' || b == '\0') {
            return (int)a - (int)b;
        }
    }
    return 0;
}

long strtol(const char *text, char **end, int base) {
    const char *cursor = text;
    int negative = 0;
    uint64_t value = 0U;
    int overflow = 0;

    if (text == NULL) {
        if (end != NULL) {
            *end = NULL;
        }
        return 0L;
    }
    if (base != 0 && base != 10 && base != 16) {
        if (end != NULL) {
            *end = (char *)cursor;
        }
        return 0L;
    }

    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' ||
           *cursor == '\r' || *cursor == '\f' || *cursor == '\v') {
        ++cursor;
    }
    if (*cursor == '-') {
        negative = 1;
        ++cursor;
    } else if (*cursor == '+') {
        ++cursor;
    }
    if (base == 0 || base == 16) {
        if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
            base = 16;
            cursor += 2;
        } else if (base == 0) {
            base = 10;
        }
    }

    for (;;) {
        unsigned int digit;
        unsigned char ch = (unsigned char)*cursor;

        if (ch >= '0' && ch <= '9') {
            digit = (unsigned int)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = (unsigned int)(ch - 'a') + 10U;
        } else if (ch >= 'A' && ch <= 'F') {
            digit = (unsigned int)(ch - 'A') + 10U;
        } else {
            break;
        }
        if (digit >= (unsigned int)base) {
            break;
        }
        if (value > (UINT64_MAX - digit) / (unsigned int)base) {
            overflow = 1;
            value = 0U;
            while ((*cursor >= '0' && *cursor <= '9') ||
                   (*cursor >= 'a' && *cursor <= 'f') ||
                   (*cursor >= 'A' && *cursor <= 'F')) {
                ++cursor;
            }
            break;
        }
        value = value * (unsigned int)base + digit;
        ++cursor;
    }

    if (end != NULL) {
        *end = (char *)cursor;
    }
    if (overflow) {
        return negative ? LONG_MIN : LONG_MAX;
    }
    if (negative) {
        return value > (uint64_t)LONG_MAX + 1U
            ? LONG_MIN : -(long)value;
    }
    return value > (uint64_t)LONG_MAX ? LONG_MAX : (long)value;
}
