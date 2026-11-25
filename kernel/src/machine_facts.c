/*
 * ITEM-521 (increment 10): render the machine record the install journal
 * carries.  The format is the one the host witness and slice 209 pin:
 *
 *   [BOOT] machine cpus=<decimal cores> memory=<decimal MiB>MiB
 *
 * The copy into the caller's buffer happens only after the whole record was
 * assembled, so a buffer that is one byte short is refused with an empty
 * answer instead of a truncated record (requirement 5: a loss is named, never
 * printed as a half line).
 */

#include "machine_facts.h"

#include <string.h>

static size_t machine_facts_append(char *record, size_t length,
                                   const char *text) {
    while (*text != '\0') {
        record[length++] = *text++;
    }
    return length;
}

static size_t machine_facts_append_u64(char *record, size_t length,
                                       uint64_t value) {
    char digits[20U];
    size_t count = 0U;

    if (value == 0U) {
        record[length++] = '0';
        return length;
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (uint32_t)(value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        record[length++] = digits[--count];
    }
    return length;
}

size_t machine_facts_record(char *output, size_t capacity, uint64_t memory_mib,
                            uint32_t cores) {
    char record[MACHINE_FACTS_RECORD_MAX];
    size_t length = 0U;

    if (output == NULL || capacity == 0U) {
        return 0U;
    }
    output[0] = '\0';
    length = machine_facts_append(record, length, "[BOOT] machine cpus=");
    length = machine_facts_append_u64(record, length, (uint64_t)cores);
    length = machine_facts_append(record, length, " memory=");
    length = machine_facts_append_u64(record, length, memory_mib);
    length = machine_facts_append(record, length, "MiB");
    if (length + 1U > capacity) {
        return 0U;
    }
    record[length] = '\0';
    memcpy(output, record, length + 1U);
    return length;
}
