#ifndef HFOC_MACHINE_FACTS_H
#define HFOC_MACHINE_FACTS_H

/*
 * ITEM-521 (increment 10): the one record the install journal keeps about the
 * machine the run happened on.
 *
 * Requirement 1 of the install journal names the machine - its cores and its
 * memory - next to the medium, the observed storage and every refusal.  The
 * queue-side dry run of the prepared placement (2026-10-12) read the kernel's
 * own journal off the emulated medium and found that half missing: the memory
 * line went to serial only and the core count was written nowhere.
 *
 * The record is a plain function so it can be proven on the host and reused by
 * whatever writes it; the emission (serial plus the durable journal, one
 * sequence) belongs to the caller.
 */

#include <stddef.h>
#include <stdint.h>

/*
 * Worst case: the two literals (28 bytes), a ten-digit core count and a
 * twenty-digit memory value, plus the terminating NUL.  A caller that sizes its
 * buffer with this constant can never be refused.
 */
#define MACHINE_FACTS_RECORD_MAX 64U

/*
 * Render `[BOOT] machine cpus=<cores> memory=<mib>MiB` into `output`.
 *
 * Returns the record length without the terminating NUL.  When `output` is
 * NULL, `capacity` is zero, or the record does not fit including its NUL, the
 * function writes nothing (an empty string when it can) and returns 0: a
 * truncated record is never handed to a journal.
 */
size_t machine_facts_record(char *output, size_t capacity, uint64_t memory_mib,
                            uint32_t cores);

#endif
