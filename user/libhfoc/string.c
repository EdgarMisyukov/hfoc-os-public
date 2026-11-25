/* ITEM-626: freestanding string and memory helpers. */
#include "hfoc.h"

size_t hfoc_strlen(const char *text)
{
    size_t length = 0U;

    if (text == NULL) {
        return 0U;
    }
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

int hfoc_strcmp(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int hfoc_strncmp(const char *left, const char *right, size_t count)
{
    while (count != 0U && *left != '\0' && *left == *right) {
        ++left;
        ++right;
        --count;
    }
    if (count == 0U) {
        return 0;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

char *hfoc_strcpy(char *destination, const char *source)
{
    char *result = destination;

    while ((*destination++ = *source++) != '\0') {
    }
    return result;
}

char *hfoc_strncpy(char *destination, const char *source, size_t count)
{
    char *result = destination;

    while (count != 0U && *source != '\0') {
        *destination++ = *source++;
        --count;
    }
    while (count != 0U) {
        *destination++ = '\0';
        --count;
    }
    return result;
}

char *hfoc_strchr(const char *text, int character)
{
    const char wanted = (char)character;

    for (;;) {
        if (*text == wanted) {
            return (char *)(uintptr_t)text;
        }
        if (*text == '\0') {
            return NULL;
        }
        ++text;
    }
}

char *hfoc_strrchr(const char *text, int character)
{
    const char wanted = (char)character;
    const char *last = NULL;

    for (;;) {
        if (*text == wanted) {
            last = text;
        }
        if (*text == '\0') {
            return (char *)(uintptr_t)last;
        }
        ++text;
    }
}

char *hfoc_strstr(const char *haystack, const char *needle)
{
    const size_t length = hfoc_strlen(needle);

    if (length == 0U) {
        return (char *)(uintptr_t)haystack;
    }
    while (*haystack != '\0') {
        if (*haystack == *needle &&
            hfoc_strncmp(haystack, needle, length) == 0) {
            return (char *)(uintptr_t)haystack;
        }
        ++haystack;
    }
    return NULL;
}

static int hfoc_lower(int character)
{
    if (character >= 'A' && character <= 'Z') {
        return character + ('a' - 'A');
    }
    return character;
}

int hfoc_strcasecmp(const char *left, const char *right)
{
    while (*left != '\0' && hfoc_lower((unsigned char)*left) ==
                                   hfoc_lower((unsigned char)*right)) {
        ++left;
        ++right;
    }
    return hfoc_lower((unsigned char)*left) -
           hfoc_lower((unsigned char)*right);
}

int hfoc_memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;

    while (count != 0U) {
        if (*a != *b) {
            return (int)*a - (int)*b;
        }
        ++a;
        ++b;
        --count;
    }
    return 0;
}

void *hfoc_memcpy(void *destination, const void *source, size_t count)
{
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;

    while (count != 0U) {
        *to++ = *from++;
        --count;
    }
    return destination;
}

void *hfoc_memmove(void *destination, const void *source, size_t count)
{
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;

    if (to == from || count == 0U) {
        return destination;
    }
    if (to < from) {
        while (count != 0U) {
            *to++ = *from++;
            --count;
        }
    } else {
        to += count;
        from += count;
        while (count != 0U) {
            *--to = *--from;
            --count;
        }
    }
    return destination;
}

void *hfoc_memset(void *destination, int value, size_t count)
{
    unsigned char *to = (unsigned char *)destination;

    while (count != 0U) {
        *to++ = (unsigned char)value;
        --count;
    }
    return destination;
}

long hfoc_parse_int(const char *text, int *ok)
{
    long value = 0L;
    int negative = 0;
    int digits = 0;

    if (ok != NULL) {
        *ok = 0;
    }
    if (text == NULL) {
        return 0L;
    }
    if (*text == '-') {
        negative = 1;
        ++text;
    } else if (*text == '+') {
        ++text;
    }
    while (*text >= '0' && *text <= '9') {
        value = value * 10L + (long)(*text - '0');
        ++text;
        ++digits;
    }
    if (digits == 0 || *text != '\0') {
        return 0L;
    }
    if (ok != NULL) {
        *ok = 1;
    }
    return negative ? -value : value;
}

#ifndef HFOC_USER_RUNTIME_HOST_TEST
/* The compiler may emit calls to these libc names for structure copies. */
void *memcpy(void *destination, const void *source, size_t count)
{
    return hfoc_memcpy(destination, source, count);
}

void *memmove(void *destination, const void *source, size_t count)
{
    return hfoc_memmove(destination, source, count);
}

void *memset(void *destination, int value, size_t count)
{
    return hfoc_memset(destination, value, count);
}

int memcmp(const void *left, const void *right, size_t count)
{
    return hfoc_memcmp(left, right, count);
}

size_t strlen(const char *text)
{
    return hfoc_strlen(text);
}

int strcmp(const char *left, const char *right)
{
    return hfoc_strcmp(left, right);
}

int strncmp(const char *left, const char *right, size_t count)
{
    return hfoc_strncmp(left, right, count);
}

char *strcpy(char *destination, const char *source)
{
    return hfoc_strcpy(destination, source);
}

char *strchr(const char *text, int character)
{
    return hfoc_strchr(text, character);
}
#endif
