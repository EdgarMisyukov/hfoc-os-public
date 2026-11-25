#include "hfoc_mbedtls_platform.h"

#ifdef HFOC_PRODUCTION_TLS

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-386 production mbedTLS platform shim.
 *
 * The frozen production mbedTLS profile runs with
 * MBEDTLS_PLATFORM_NO_STD_FUNCTIONS, because the freestanding kernel links no
 * libc.  The bounded formatting implementation below is the same one the
 * ITEM-270 QEMU TLS fixture proved over a real TLS 1.3 handshake; ITEM-386
 * moves it into a production translation unit so the ordinary production
 * image can format the X.509 validity window and the human-readable
 * certificate fields mbedTLS writes at first boot.  Nothing here allocates,
 * nothing here recurses, and every write is bounded by the caller's capacity.
 *
 * mbedtls_platform_zeroize is deliberately NOT defined here: the production
 * image takes exactly one definition from image_signature_ecdsa.c, and both
 * profiles declare MBEDTLS_PLATFORM_ZEROIZE_ALT.
 */

/*
 * mbedTLS 3.6.7 X.509 and PK parsing uses these three libc helpers, which the
 * freestanding kernel does not provide.  Each one is bounded by its input and
 * by a compile-time maximum, and each returns the same value the C library
 * would for the inputs mbedTLS produces.
 */
#define HFOC_MBEDTLS_PLATFORM_STRING_MAX 4096U

char *strchr(const char *text, int character)
{
    unsigned char wanted = (unsigned char)character;

    if (text == NULL) {
        return NULL;
    }
    for (;;) {
        if ((unsigned char)*text == wanted) {
            return (char *)text;
        }
        if (*text == '\0') {
            return NULL;
        }
        ++text;
    }
}

char *strstr(const char *text, const char *needle)
{
    size_t index;

    if (text == NULL || needle == NULL) {
        return NULL;
    }
    if (*needle == '\0') {
        return (char *)text;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        size_t offset = 0U;

        if (index >= HFOC_MBEDTLS_PLATFORM_STRING_MAX) {
            return NULL;
        }
        while (needle[offset] != '\0' &&
               text[index + offset] == needle[offset]) {
            ++offset;
            if (index + offset >= HFOC_MBEDTLS_PLATFORM_STRING_MAX) {
                return NULL;
            }
        }
        if (needle[offset] == '\0') {
            return (char *)&text[index];
        }
    }
    return NULL;
}

/*
 * mbedTLS 3.6.7 X.509 address matching calls the platform inet_pton() when the
 * toolchain headers expose AF_INET/AF_INET6.  The freestanding kernel links no
 * libc, so the bounded implementation lives here: IPv4 and IPv6 with "::"
 * compression, no allocation, no recursion, and a hard limit on the input it
 * will look at.
 */
#define HFOC_TLS_PLATFORM_AF_INET 2
#define HFOC_TLS_PLATFORM_AF_INET6 10
#define HFOC_TLS_PLATFORM_ADDRESS_TEXT_MAX 64U

static int hfoc_tls_platform_hex_digit(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static int hfoc_tls_platform_parse_ipv4(
    const char *text,
    unsigned char *destination)
{
    uint32_t index = 0U;
    uint32_t part;

    for (part = 0U; part < 4U; ++part) {
        uint32_t value = 0U;
        uint32_t digits = 0U;

        while (text[index] >= '0' && text[index] <= '9' && digits < 3U) {
            value = value * 10U + (uint32_t)(text[index] - '0');
            ++index;
            ++digits;
        }
        if (digits == 0U || value > 255U) {
            return 0;
        }
        destination[part] = (unsigned char)value;
        if (part == 3U) {
            break;
        }
        if (text[index] != '.') {
            return 0;
        }
        ++index;
    }
    return text[index] == '\0' ? 1 : 0;
}

static int hfoc_tls_platform_parse_ipv6(
    const char *text,
    unsigned char *destination)
{
    uint16_t groups[8];
    uint32_t count = 0U;
    uint32_t zero_at = 8U;
    uint32_t index = 0U;
    uint32_t group;

    for (group = 0U; group < 8U; ++group) {
        groups[group] = 0U;
    }
    if (text[0] == ':' && text[1] == ':') {
        zero_at = 0U;
        index = 2U;
    } else if (text[0] == ':') {
        return 0;
    }
    while (text[index] != '\0') {
        uint32_t value = 0U;
        uint32_t digits = 0U;

        if (count >= 8U) {
            return 0;
        }
        while (digits < 4U) {
            int digit = hfoc_tls_platform_hex_digit(text[index]);

            if (digit < 0) {
                break;
            }
            value = (value << 4U) | (uint32_t)digit;
            ++index;
            ++digits;
        }
        if (digits == 0U) {
            return 0;
        }
        groups[count++] = (uint16_t)value;
        if (text[index] == '\0') {
            break;
        }
        if (text[index] != ':') {
            return 0;
        }
        ++index;
        if (text[index] == ':') {
            if (zero_at != 8U) {
                return 0;
            }
            zero_at = count;
            ++index;
            if (text[index] == '\0') {
                break;
            }
        }
    }
    if (zero_at == 8U) {
        if (count != 8U) {
            return 0;
        }
    } else {
        uint32_t tail = count - zero_at;

        if (count >= 8U) {
            return 0;
        }
        for (group = 0U; group < tail; ++group) {
            groups[7U - group] = groups[count - 1U - group];
        }
        for (group = zero_at; group < 8U - tail; ++group) {
            groups[group] = 0U;
        }
    }
    for (group = 0U; group < 8U; ++group) {
        destination[group * 2U] = (unsigned char)(groups[group] >> 8U);
        destination[group * 2U + 1U] = (unsigned char)(groups[group] & 0xFFU);
    }
    return 1;
}

int inet_pton(int family, const char *source, void *destination)
{
    uint32_t index;

    if (source == NULL || destination == NULL) {
        return 0;
    }
    for (index = 0U; index < HFOC_TLS_PLATFORM_ADDRESS_TEXT_MAX; ++index) {
        if (source[index] == '\0') {
            break;
        }
    }
    if (index >= HFOC_TLS_PLATFORM_ADDRESS_TEXT_MAX) {
        return 0;
    }
    if (family == HFOC_TLS_PLATFORM_AF_INET) {
        return hfoc_tls_platform_parse_ipv4(source, destination);
    }
    if (family == HFOC_TLS_PLATFORM_AF_INET6) {
        return hfoc_tls_platform_parse_ipv6(source, destination);
    }
    return 0;
}

char *strncpy(char *destination, const char *source, size_t count)
{
    size_t index = 0U;

    if (destination == NULL || source == NULL) {
        return destination;
    }
    while (index < count && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    while (index < count) {
        destination[index] = '\0';
        ++index;
    }
    return destination;
}

static void hfoc_tls_platform_putc(
    char *buffer,
    size_t capacity,
    size_t *position,
    char value)
{
    if (*position < capacity) {
        buffer[*position] = value;
    }
    ++*position;
}

static void hfoc_tls_platform_puts(
    char *buffer,
    size_t capacity,
    size_t *position,
    const char *text,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        hfoc_tls_platform_putc(buffer, capacity, position, text[index]);
    }
}


static void hfoc_tls_platform_pad(
    char *buffer,
    size_t capacity,
    size_t *position,
    unsigned int width,
    bool left,
    char fill)
{
    unsigned int index;

    for (index = 0U; index < width; ++index) {
        hfoc_tls_platform_putc(buffer, capacity, position, fill);
    }
    (void)left;
}

static void hfoc_tls_platform_format_number(
    char *buffer,
    size_t capacity,
    size_t *position,
    uint64_t value,
    bool negative,
    unsigned int base,
    bool uppercase,
    bool left,
    char fill,
    unsigned int width,
    int precision)
{
    char digits[64];
    size_t digit_count = 0U;
    unsigned int padding;

    do {
        unsigned int digit = (unsigned int)(value % base);

        digits[digit_count++] = (char)(
            digit < 10U ? '0' + digit :
            (uppercase ? 'A' : 'a') + digit - 10U);
        value /= base;
    } while (value != 0U);

    if (precision > (int)digit_count) {
        while (digit_count < (size_t)precision) {
            digits[digit_count++] = '0';
        }
    }
    if (negative) {
        hfoc_tls_platform_putc(buffer, capacity, position, '-');
    }
    padding = width > digit_count ? width - (unsigned int)digit_count : 0U;
    if (!left && fill == '0') {
        hfoc_tls_platform_pad(
            buffer, capacity, position, padding, false, fill);
        padding = 0U;
    }
    while (digit_count != 0U) {
        --digit_count;
        hfoc_tls_platform_putc(
            buffer, capacity, position, digits[digit_count]);
    }
    if (left) {
        hfoc_tls_platform_pad(
            buffer, capacity, position, padding, true, ' ');
    }
}

int hfoc_mbedtls_vsnprintf(
    char *s,
    size_t n,
    const char *format,
    va_list arguments);

int hfoc_mbedtls_snprintf(
    char *s,
    size_t n,
    const char *format,
    ...)
{
    va_list arguments;
    int result;

    va_start(arguments, format);
    result = hfoc_mbedtls_vsnprintf(s, n, format, arguments);
    va_end(arguments);
    return result;
}

int hfoc_mbedtls_vsnprintf(
    char *s,
    size_t n,
    const char *format,
    va_list arguments)
{
    size_t position = 0U;
    const char *cursor;

    if (s == NULL || (n != 0U && format == NULL)) {
        return 0;
    }

    for (cursor = format; *cursor != '\0'; ++cursor) {
        bool left = false;
        bool zero_pad = false;
        unsigned int width = 0U;
        int precision = -1;
        bool width_star = false;
        bool precision_star = false;
        char length = '\0';
        char conversion;

        if (*cursor != '%') {
            hfoc_tls_platform_putc(s, n, &position, *cursor);
            continue;
        }
        ++cursor;
        if (*cursor == '\0') {
            break;
        }
        if (*cursor == '%') {
            hfoc_tls_platform_putc(s, n, &position, '%');
            continue;
        }
        while (*cursor == '-' || *cursor == '0' || *cursor == '+' ||
               *cursor == ' ' || *cursor == '#') {
            if (*cursor == '-') {
                left = true;
            } else if (*cursor == '0') {
                zero_pad = true;
            }
            ++cursor;
        }
        if (*cursor == '*') {
            width_star = true;
            ++cursor;
        } else {
            while (*cursor >= '0' && *cursor <= '9') {
                width = width * 10U + (unsigned int)(*cursor - '0');
                ++cursor;
            }
        }
        if (*cursor == '.') {
            ++cursor;
            if (*cursor == '*') {
                precision_star = true;
                ++cursor;
            } else {
                precision = 0;
                while (*cursor >= '0' && *cursor <= '9') {
                    precision = precision * 10 + (*cursor - '0');
                    ++cursor;
                }
            }
        }
        if (*cursor == 'h' || *cursor == 'l' || *cursor == 'z' ||
            *cursor == 'j' || *cursor == 't') {
            length = *cursor;
            ++cursor;
            if (*cursor == 'l' && length == 'l') {
                length = 'L';
                ++cursor;
            }
        }
        if (*cursor == '\0') {
            break;
        }
        conversion = *cursor;
        if (width_star) {
            int star_width = va_arg(arguments, int);

            if (star_width < 0) {
                left = true;
                width = (unsigned int)(-star_width);
            } else {
                width = (unsigned int)star_width;
            }
        }
        if (precision_star) {
            precision = va_arg(arguments, int);
        }

        switch (conversion) {
        case 'c': {
            int value = va_arg(arguments, int);

            hfoc_tls_platform_putc(s, n, &position, (char)value);
            break;
        }
        case 's': {
            const char *text = va_arg(arguments, const char *);
            size_t text_length = 0U;
            unsigned int text_padding;

            if (text == NULL) {
                text = "(null)";
            }
            while (text[text_length] != '\0') {
                ++text_length;
            }
            if (precision >= 0 &&
                text_length > (size_t)precision) {
                text_length = (size_t)precision;
            }
            text_padding = width > text_length ?
                width - (unsigned int)text_length : 0U;
            if (!left) {
                hfoc_tls_platform_pad(
                    s, n, &position, text_padding, false, ' ');
                text_padding = 0U;
            }
            hfoc_tls_platform_puts(s, n, &position, text, text_length);
            if (left) {
                hfoc_tls_platform_pad(
                    s, n, &position, text_padding, true, ' ');
            }
            break;
        }
        case 'd':
        case 'i': {
            int64_t signed_value;
            uint64_t value;
            bool negative = false;

            if (length == 'l' || length == 'L') {
                signed_value = va_arg(arguments, int64_t);
            } else {
                signed_value = (int64_t)va_arg(arguments, int);
            }
            if (signed_value < 0) {
                negative = true;
                value = (uint64_t)(-signed_value);
            } else {
                value = (uint64_t)signed_value;
            }
            hfoc_tls_platform_format_number(
                s, n, &position, value, negative, 10U, false,
                left, zero_pad ? '0' : ' ', width, precision);
            break;
        }
        case 'u': {
            uint64_t value;

            if (length == 'l' || length == 'L') {
                value = va_arg(arguments, uint64_t);
            } else if (length == 'z') {
                value = (uint64_t)va_arg(arguments, size_t);
            } else {
                value = (uint64_t)va_arg(arguments, unsigned int);
            }
            hfoc_tls_platform_format_number(
                s, n, &position, value, false, 10U, false,
                left, zero_pad ? '0' : ' ', width, precision);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t value;

            if (length == 'l' || length == 'L') {
                value = va_arg(arguments, uint64_t);
            } else if (length == 'z') {
                value = (uint64_t)va_arg(arguments, size_t);
            } else {
                value = (uint64_t)va_arg(arguments, unsigned int);
            }
            hfoc_tls_platform_format_number(
                s, n, &position, value, false, 16U,
                conversion == 'X', left, zero_pad ? '0' : ' ',
                width, precision);
            break;
        }
        case 'p': {
            uintptr_t value =
                (uintptr_t)va_arg(arguments, void *);

            hfoc_tls_platform_puts(s, n, &position, "0x", 2U);
            hfoc_tls_platform_format_number(
                s, n, &position, (uint64_t)value, false, 16U,
                false, false, '0', 0U, -1);
            break;
        }
        default:
            hfoc_tls_platform_putc(s, n, &position, conversion);
            break;
        }
    }

    if (n != 0U) {
        if (position < n) {
            s[position] = '\0';
        } else {
            s[n - 1U] = '\0';
        }
    }
    return (int)position;
}

#else

typedef int appliance_tls_platform_translation_unit_guard;

#endif
