#include "kernel_status_format.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

int main(void)
{
    char text[32];
    size_t length;

    memset(text, 'x', sizeof(text));
    length = kernel_status_append_hex16(text, sizeof(text), 0U, 0x10ECU);
    CHECK(length == 6U && strcmp(text, "0x10EC") == 0,
          "the PCI vendor id is four hex digits, not five");
    length = kernel_status_append_hex16(text, sizeof(text), 0U, 0x8168U);
    CHECK(length == 6U && strcmp(text, "0x8168") == 0,
          "the PCI device id is four hex digits");
    length = kernel_status_append_hex16(text, sizeof(text), 0U, 0x0001U);
    CHECK(length == 6U && strcmp(text, "0x0001") == 0,
          "leading zeroes are kept so every id has the same width");
    length = kernel_status_append_hex16(text, sizeof(text), 0U, 0xABCDU);
    CHECK(length == 6U && strcmp(text, "0xABCD") == 0,
          "all four digits are emitted in the right order");

    length = kernel_status_append_hex16(text, sizeof(text), 0U, 0x0000U);
    CHECK(length == 6U && strcmp(text, "0x0000") == 0,
          "zero is still four digits");
    strcpy(text, "pci=");
    length = kernel_status_append_hex16(text, sizeof(text), 4U, 0x10ECU);
    CHECK(length == 10U && strcmp(text, "pci=0x10EC") == 0,
          "the formatter appends at the caller's offset");

    memset(text, 'x', sizeof(text));
    length = kernel_status_append_hex16(text, 6U, 0U, 0x10ECU);
    CHECK(length <= 5U && text[5] == '\0',
          "a short buffer is bounded and terminated, never overrun");
    CHECK(kernel_status_append_hex16(NULL, sizeof(text), 0U, 0x10ECU) == 0U,
          "a missing buffer is refused");

    if (failures != 0) {
        fprintf(stderr, "kernel status format host: %d failures\n", failures);
        return 1;
    }
    puts("kernel status format host: ALL OK");
    return 0;
}
