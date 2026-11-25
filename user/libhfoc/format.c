/* ITEM-626: a bounded formatter for the native user programs. */
#include "hfoc.h"

#include <stdarg.h>

struct format_out {
    char *buffer;
    size_t capacity;
    size_t used;
};

static void emit_char(struct format_out *out, char character)
{
    if (out->capacity != 0U && out->used + 1U < out->capacity) {
        out->buffer[out->used] = character;
    }
    ++out->used;
}

static void emit_text(struct format_out *out, const char *text)
{
    if (text == NULL) {
        text = "(null)";
    }
    while (*text != '\0') {
        emit_char(out, *text++);
    }
}

static void emit_unsigned(struct format_out *out, unsigned long long value,
                          unsigned base, int uppercase, int width, int zero,
                          int left)
{
    char digits[32];
    const char *alphabet = uppercase ? "0123456789ABCDEF"
                                     : "0123456789abcdef";
    int count = 0;
    int padding;

    if (value == 0ULL) {
        digits[count++] = '0';
    }
    while (value != 0ULL) {
        digits[count++] = alphabet[value % base];
        value /= base;
    }
    padding = width > count ? width - count : 0;
    if (!left) {
        while (padding-- > 0) {
            emit_char(out, zero ? '0' : ' ');
        }
    }
    while (count > 0) {
        emit_char(out, digits[--count]);
    }
    if (left) {
        while (padding-- > 0) {
            emit_char(out, ' ');
        }
    }
}

static void emit_signed(struct format_out *out, long long value, int width,
                        int zero, int left)
{
    unsigned long long magnitude;
    int negative = value < 0;
    int padding;

    magnitude = negative ? (unsigned long long)(-(value + 1LL)) + 1ULL
                         : (unsigned long long)value;
    {
        char digits[32];
        int count = 0;
        int extra = negative ? 1 : 0;

        if (magnitude == 0ULL) {
            digits[count++] = '0';
        }
        while (magnitude != 0ULL) {
            digits[count++] = (char)('0' + (int)(magnitude % 10ULL));
            magnitude /= 10ULL;
        }
        padding = width > count + extra ? width - count - extra : 0;
        if (!left) {
            if (zero) {
                if (negative) {
                    emit_char(out, '-');
                    negative = 0;
                }
                while (padding-- > 0) {
                    emit_char(out, '0');
                }
            } else {
                while (padding-- > 0) {
                    emit_char(out, ' ');
                }
            }
        }
        if (negative) {
            emit_char(out, '-');
        }
        while (count > 0) {
            emit_char(out, digits[--count]);
        }
        if (left) {
            while (padding-- > 0) {
                emit_char(out, ' ');
            }
        }
    }
}

int hfoc_vsnprintf(char *buffer, size_t capacity, const char *format,
                   va_list arguments)
{
    struct format_out out = { buffer, capacity, 0U };

    while (*format != '\0') {
        int left = 0;
        int zero = 0;
        int width = 0;

        if (*format != '%') {
            emit_char(&out, *format++);
            continue;
        }
        ++format;
        if (*format == '%') {
            emit_char(&out, '%');
            ++format;
            continue;
        }
        for (;;) {
            if (*format == '-') {
                left = 1;
                ++format;
            } else if (*format == '0') {
                zero = 1;
                ++format;
            } else {
                break;
            }
        }
        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            ++format;
        }
        if (*format == 'l') {
            ++format;
            if (*format == 'l') {
                ++format;
            }
        } else if (*format == 'z') {
            ++format;
        }
        switch (*format) {
        case 'c':
            emit_char(&out, (char)va_arg(arguments, int));
            break;
        case 's':
            emit_text(&out, va_arg(arguments, const char *));
            break;
        case 'd':
        case 'i':
            emit_signed(&out, (long long)va_arg(arguments, int), width, zero,
                        left);
            break;
        case 'u':
            emit_unsigned(&out, (unsigned long long)va_arg(arguments,
                                                           unsigned int),
                          10U, 0, width, zero, left);
            break;
        case 'x':
            emit_unsigned(&out, (unsigned long long)va_arg(arguments,
                                                           unsigned int),
                          16U, 0, width, zero, left);
            break;
        case 'X':
            emit_unsigned(&out, (unsigned long long)va_arg(arguments,
                                                           unsigned int),
                          16U, 1, width, zero, left);
            break;
        case 'p':
            emit_text(&out, "0x");
            emit_unsigned(&out, (unsigned long long)(uintptr_t)va_arg(
                                    arguments, void *),
                          16U, 0, width, zero, left);
            break;
        case '\0':
            --format;
            break;
        default:
            emit_char(&out, '%');
            emit_char(&out, *format);
            break;
        }
        ++format;
    }
    if (capacity != 0U) {
        const size_t end = out.used < capacity ? out.used : capacity - 1U;

        buffer[end] = '\0';
    }
    return (int)out.used;
}

int hfoc_snprintf(char *buffer, size_t capacity, const char *format, ...)
{
    va_list arguments;
    int result;

    va_start(arguments, format);
    result = hfoc_vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
    return result;
}
