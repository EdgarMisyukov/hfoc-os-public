#ifndef HFOC_PANIC_RECORD_H
#define HFOC_PANIC_RECORD_H

/*
 * ITEM-521 (increment 11): the panic names itself in the durable journal.
 *
 * Requirement 1 of the owner's item ("носитель ведёт журнал установки") lists
 * what the medium's journal has to carry, and names the panic next to the
 * medium, the machine, the observed storage, the flow's decisions and every
 * refusal; requirement 2 adds that the panic path has to get its tail out.
 *
 * Both halves were wired before this increment - the panic path flushes the
 * staged records and the seam that reaches the medium - but the panic itself
 * was never a record: `panic_finish()` wrote the message to the serial stream
 * and the screen, and then flushed the *older* records it had not written yet.
 * A machine with no serial port therefore ends its journal on whatever ran
 * before the panic, and the one thing the owner needs to read - that the run
 * stopped, and why - is exactly the thing missing (the same shape of defect
 * increment 10 found for the machine record: found by reading the journal off
 * the emulated medium, not by reading the code).
 *
 * The renderers here are pure functions so the exact records can be proven on
 * the host before any medium is touched; the emission (the durable boot-log
 * path, then the two flushes) belongs to `panic.c`.
 *
 * Two records, not one, because a panic detail may be nearly as long as a
 * journal slot: the message record always carries the message and the frame
 * facts, and the detail record carries the detail when there is one.  Both fit
 * the shared record bound (APPLIANCE_BOOT_LOG_TEXT_MAX, 440) with room to
 * spare, so neither the store nor the journal has to clamp them.
 *
 * Requirement 5 applies to a panic record like to any other: a loss is named.
 * A message longer than PANIC_RECORD_MESSAGE_MAX is clamped with a visible
 * `...` marker, exactly as the durable log clamps its own over-long records,
 * and a buffer that cannot hold the whole record is refused with an empty
 * answer rather than handed a half line.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The longest message a panic record carries before the clamp marks it. */
#define PANIC_RECORD_MESSAGE_MAX 160U

/* The longest detail a detail record carries before the clamp marks it; it is
 * the kernel's own bounded panic detail line (PANIC_DETAIL_MAX - 1). */
#define PANIC_RECORD_DETAIL_MAX 399U

/*
 * Worst case of the message record: the `[PANIC] ` prefix (8), a clamped
 * message (160), the widest frame fields - ` vector=` (8) plus 20 digits,
 * ` error=0x` (9) plus 16 hex digits, ` rip=0x` (7) plus 16 hex digits - and
 * the terminating NUL.
 */
#define PANIC_RECORD_MAX 256U

/*
 * Worst case of the detail record: `[PANIC] detail=` (15), a clamped detail
 * (399) and the terminating NUL.
 */
#define PANIC_RECORD_DETAIL_LINE_MAX 416U

/*
 * Render `[PANIC] <message> vector=<decimal> error=0x<hex> rip=0x<hex>` into
 * `output`.  The frame fields are appended only when `has_frame`; a NULL
 * message renders as `Unknown panic`, the same words the serial stream uses.
 *
 * Returns the record length without the terminating NUL.  When `output` is
 * NULL, `capacity` is zero, or the record does not fit including its NUL, the
 * function writes nothing (an empty string when it can) and returns 0.
 */
size_t panic_record_render(char *output, size_t capacity, const char *message,
                           bool has_frame, uint64_t vector,
                           uint64_t error_code, uint64_t rip);

/*
 * Render `[PANIC] detail=<detail>` into `output`.
 *
 * Returns the record length without the terminating NUL, or 0 when there is no
 * detail to render (NULL or empty), when `output`/`capacity` cannot hold the
 * record, or when `capacity` is zero - the same refusal rule as the message
 * record: nothing is ever handed out truncated.
 */
size_t panic_record_detail(char *output, size_t capacity, const char *detail);

#endif
