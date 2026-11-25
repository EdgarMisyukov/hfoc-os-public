#ifndef HFOC_PIPE_H
#define HFOC_PIPE_H

#include "process.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-627: bounded pipes over the existing descriptor/VFS owners.
 *
 * A pipe is one 4 KiB ring with one read end and one write end.  The ends are
 * reference-counted so dup2 and descriptor inheritance share them; the pipe is
 * released when both directions are gone.  Readers block on the read queue and
 * writers on the write queue; a timed wait in the descriptor loops means a lost
 * wake cannot deadlock the process, and the network loop is never entered from
 * inside the pipe.
 */
#define HFOC_PIPE_MAX 8U
#define HFOC_PIPE_CAPACITY 4096U
#define HFOC_PIPE_WAIT_TICKS 100U

enum pipe_status {
    PIPE_OK = 0,
    PIPE_NULL = 1,
    PIPE_NO_SPACE = 2,
    PIPE_BROKEN = 3,
    PIPE_WOULD_BLOCK = 4,
    PIPE_BAD_STATE = 5,
    PIPE_NO_DESCRIPTOR = 6,
};

struct hfoc_pipe;

struct hfoc_pipe_end {
    struct hfoc_pipe *pipe;
    bool writer;
    uint32_t refs;
};

struct hfoc_pipe {
    bool used;
    uint8_t data[HFOC_PIPE_CAPACITY];
    uint32_t head;
    uint32_t count;
    uint32_t readers;
    uint32_t writers;
    struct scheduler_wait_queue read_wait;
    struct scheduler_wait_queue write_wait;
    struct hfoc_pipe_end read_end;
    struct hfoc_pipe_end write_end;
};

enum pipe_status pipe_create(struct hfoc_pipe **pipe_out);
uint32_t pipe_pending(const struct hfoc_pipe *pipe);
uint32_t pipe_free_space(const struct hfoc_pipe *pipe);

/* Non-blocking core: the host proof and the descriptor ops both use it. */
enum pipe_status pipe_read_now(struct hfoc_pipe *pipe, void *buffer,
                               size_t size, size_t *done_out);
enum pipe_status pipe_write_now(struct hfoc_pipe *pipe, const void *buffer,
                                size_t size, size_t *done_out);
void pipe_end_retain(struct hfoc_pipe_end *end);
void pipe_end_release(struct hfoc_pipe_end *end);

/* Bind one end to the next free descriptor of a user process. */
enum pipe_status pipe_bind_fd(struct kernel_process *process,
                              struct hfoc_pipe *pipe, bool writer,
                              uint32_t *descriptor_out);

/*
 * ITEM-628: spawn actions adopt a pipe end the parent already holds.  The
 * child gets its own description storage pointing at the same reference-
 * counted end, so closing the parent copy cannot free the child's pipe.
 */
bool pipe_description_is_pipe(const struct file_description *description);
enum pipe_status pipe_adopt_fd(struct kernel_process *process,
                               const struct file_description *source,
                               uint32_t descriptor);

#endif
