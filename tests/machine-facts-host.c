/*
 * ITEM-521 (increment 10): host proof for the machine facts the install
 * journal has to carry.
 *
 * Requirement 1 lists what the medium's journal must hold: the medium itself,
 * the machine (its cores and its memory), the observed storage, the flow's
 * decisions, every refusal with its reason, and the panic tail.  The queue-side
 * dry run of the prepared placement (2026-10-12, the wired extent proof on the
 * current main) read the kernel's own journal off the emulated stick and found
 * the machine half missing: record 1 was `[BOOT] Bootloader:`, the memory line
 * (`[MEM ] Usable memory: 474 MiB`) existed on serial only, and the core count
 * was written nowhere at all.
 *
 * This witness pins the renderer, not the emission: the exact record text, the
 * boundary values of both counters, the too-small buffer (nothing written, an
 * empty answer instead of a truncated record) and the bytes around the output
 * staying untouched.
 */

#include "machine_facts.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
static unsigned failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));          \
        }                                                                    \
    } while (0)

#define CANARY 0xA5U
#define CANARY_BYTES 8U

static void fill_canary(unsigned char *buffer, size_t length) {
    memset(buffer, CANARY, length);
}

static bool canary_intact(const unsigned char *buffer, size_t from,
                          size_t length) {
    for (size_t index = from; index < length; ++index) {
        if (buffer[index] != CANARY) {
            return false;
        }
    }
    return true;
}

int main(void) {
    /* 1. The one line the journal keeps about the machine. */
    {
        char record[MACHINE_FACTS_RECORD_MAX];
        const size_t length =
            machine_facts_record(record, sizeof(record), 474U, 1U);
        CHECK(length == strlen("[BOOT] machine cpus=1 memory=474MiB"),
              "the record length is the rendered length");
        CHECK(strcmp(record, "[BOOT] machine cpus=1 memory=474MiB") == 0,
              "the machine record names the cores and the memory");
        CHECK(strchr(record, '\n') == NULL,
              "the record is one line without a transport newline");
    }

    /* 2. Both counters keep their full width: the biggest core count and the
          biggest memory value are rendered, not clamped or truncated. */
    {
        char record[MACHINE_FACTS_RECORD_MAX];
        const size_t length = machine_facts_record(
            record, sizeof(record), UINT64_MAX, UINT32_MAX);
        CHECK(strcmp(record,
                     "[BOOT] machine cpus=4294967295 "
                     "memory=18446744073709551615MiB") == 0,
              "the record renders the boundary values in full");
        CHECK(length == strlen(record),
              "the returned length matches the boundary record");
    }

    /* 3. Zero is a value, not a reason to omit the field: a machine that says
          nothing is still named (the call site clamps the core count, the
          renderer does not invent one). */
    {
        char record[MACHINE_FACTS_RECORD_MAX];
        (void)machine_facts_record(record, sizeof(record), 0U, 0U);
        CHECK(strcmp(record, "[BOOT] machine cpus=0 memory=0MiB") == 0,
              "zero memory and zero cores are rendered as values");
    }

    /* 4. A buffer that cannot hold the record writes nothing and answers 0;
          one byte more is enough.  The caller never gets a truncated record. */
    {
        const size_t need = strlen("[BOOT] machine cpus=1 memory=474MiB");
        char exact[MACHINE_FACTS_RECORD_MAX];
        char short_buffer[MACHINE_FACTS_RECORD_MAX];
        short_buffer[0] = 'x';
        CHECK(machine_facts_record(exact, need + 1U, 474U, 1U) == need,
              "the exact-fit buffer takes the record");
        CHECK(machine_facts_record(short_buffer, need, 474U, 1U) == 0U,
              "a buffer one byte short is refused");
        CHECK(short_buffer[0] == '\0',
              "the refused buffer is emptied, not left holding a fragment");
        CHECK(machine_facts_record(short_buffer, 0U, 474U, 1U) == 0U,
              "a zero-capacity buffer is refused");
        CHECK(machine_facts_record(NULL, sizeof(exact), 474U, 1U) == 0U,
              "a NULL buffer is refused");
    }

    /* 5. The renderer writes only inside its buffer, and only up to the
          terminating NUL. */
    {
        unsigned char buffer[MACHINE_FACTS_RECORD_MAX + CANARY_BYTES];
        fill_canary(buffer, sizeof(buffer));
        const size_t length = machine_facts_record(
            (char *)buffer, MACHINE_FACTS_RECORD_MAX, 474U, 1U);
        CHECK(length != 0U, "the canary run renders");
        CHECK(buffer[length] == '\0', "the record is NUL-terminated");
        CHECK(canary_intact(buffer, length + 1U, sizeof(buffer)),
              "nothing past the terminating NUL is touched");
    }

    /* 6. The renderer is pure: the same facts give the same record. */
    {
        char first[MACHINE_FACTS_RECORD_MAX];
        char second[MACHINE_FACTS_RECORD_MAX];
        (void)machine_facts_record(first, sizeof(first), 8192U, 8U);
        (void)machine_facts_record(second, sizeof(second), 8192U, 8U);
        CHECK(strcmp(first, second) == 0,
              "the same facts render the same record");
    }

    printf("machine-facts-host: %u checks, %u failures\n", checks, failures);
    return failures == 0U ? 0 : 1;
}
