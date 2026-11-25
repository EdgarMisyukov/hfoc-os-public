#ifndef HFOC_TTY_H
#define HFOC_TTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "descriptor.h"
#include "scheduler.h"

/*
 * ITEM-629: a bounded, session-owned terminal for the enrolled owner's SSH
 * channel.  It is deliberately not the framebuffer or serial console: input
 * arrives only from one authenticated SSH channel, and output leaves only
 * through that channel.  The line editor lives in userland `sh`; this owner
 * keeps bytes, blocking and backpressure.
 */
#define HFOC_TTY_INPUT_MAX 4096U
#define HFOC_TTY_OUTPUT_MAX 16384U
#define HFOC_TTY_MAX 2U
#define HFOC_TTY_WAIT_TICKS 100U

enum tty_status {
    TTY_OK = 0,
    TTY_NULL = 1,
    TTY_NO_SPACE = 2,
    TTY_FULL = 3,
    TTY_HUNGUP = 4,
    TTY_AGAIN = 5,
    TTY_BAD_STATE = 6,
    TTY_NO_DESCRIPTOR = 7,
};

struct tty;
struct kernel_process;

enum tty_status tty_create(struct tty **tty_out);
void tty_destroy(struct tty *tty);
void tty_hangup(struct tty *tty);
bool tty_hung_up(const struct tty *tty);

/* Non-blocking producer side used by the SSH adapter. */
enum tty_status tty_input(struct tty *tty, const void *bytes, size_t length,
                          size_t *accepted_out);
void tty_input_eof(struct tty *tty);

/* Non-blocking core used by host proofs and descriptor loops. */
enum tty_status tty_read_now(struct tty *tty, void *buffer, size_t length,
                             size_t *done_out);
enum tty_status tty_write_now(struct tty *tty, const void *bytes,
                              size_t length, size_t *done_out);
enum tty_status tty_output_take(struct tty *tty, void *buffer, size_t capacity,
                                size_t *done_out);
uint32_t tty_input_pending(const struct tty *tty);
uint32_t tty_input_free(const struct tty *tty);
uint32_t tty_output_pending(const struct tty *tty);

/* ITEM-633: foreground process group for job control.  0 means no job is
   foreground, so control bytes stay in the byte stream for the shell editor. */
void tty_set_foreground(struct tty *tty, uint32_t pgid);
uint32_t tty_foreground(const struct tty *tty);

void tty_resize(struct tty *tty, uint32_t columns, uint32_t rows);
uint32_t tty_columns(const struct tty *tty);
uint32_t tty_rows(const struct tty *tty);

/*
 * Bind the terminal to a child's standard descriptor before it is published
 * to the scheduler.  The child owns an independent description slot, so its
 * lifetime cannot outlive the shell's descriptor storage.
 */
bool tty_description_is_tty(const struct file_description *description);
enum tty_status tty_adopt_fd(struct kernel_process *process,
                             const struct tty *source, uint32_t descriptor);

#endif
