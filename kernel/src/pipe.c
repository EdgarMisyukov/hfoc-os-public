/*
 * ITEM-627: bounded pipe ring and the two shared ends.
 */
#include "pipe.h"

#include "descriptor.h"
#include "scheduler.h"
#include "vfs.h"

#include <string.h>

#ifndef HFOC_PIPE_HOST_TEST
#include "vfs_fd.h"
#endif

static struct hfoc_pipe pipes[HFOC_PIPE_MAX];

static struct hfoc_pipe *pipe_alloc(void)
{
    uint32_t index;

    for (index = 0U; index < HFOC_PIPE_MAX; ++index) {
        if (!pipes[index].used) {
            return &pipes[index];
        }
    }
    return NULL;
}

static void pipe_wait_init(struct scheduler_wait_queue *queue)
{
#ifdef HFOC_PIPE_HOST_TEST
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0U;
#else
    (void)sched_wait_queue_init(queue);
#endif
}

static void pipe_wake_readers(struct hfoc_pipe *pipe)
{
#ifndef HFOC_PIPE_HOST_TEST
    (void)sched_wake_all(&pipe->read_wait);
#else
    (void)pipe;
#endif
}

static void pipe_wake_writers(struct hfoc_pipe *pipe)
{
#ifndef HFOC_PIPE_HOST_TEST
    (void)sched_wake_all(&pipe->write_wait);
#else
    (void)pipe;
#endif
}

enum pipe_status pipe_create(struct hfoc_pipe **pipe_out)
{
    struct hfoc_pipe *pipe;

    if (pipe_out == NULL) {
        return PIPE_NULL;
    }
    *pipe_out = NULL;
    pipe = pipe_alloc();
    if (pipe == NULL) {
        return PIPE_NO_SPACE;
    }
    memset(pipe, 0, sizeof(*pipe));
    pipe->used = true;
    pipe->readers = 1U;
    pipe->writers = 1U;
    pipe_wait_init(&pipe->read_wait);
    pipe_wait_init(&pipe->write_wait);
    pipe->read_end.pipe = pipe;
    pipe->read_end.writer = false;
    pipe->read_end.refs = 1U;
    pipe->write_end.pipe = pipe;
    pipe->write_end.writer = true;
    pipe->write_end.refs = 1U;
    *pipe_out = pipe;
    return PIPE_OK;
}

uint32_t pipe_pending(const struct hfoc_pipe *pipe)
{
    return pipe != NULL ? pipe->count : 0U;
}

uint32_t pipe_free_space(const struct hfoc_pipe *pipe)
{
    return pipe != NULL ? HFOC_PIPE_CAPACITY - pipe->count : 0U;
}

enum pipe_status pipe_read_now(struct hfoc_pipe *pipe, void *buffer,
                               size_t size, size_t *done_out)
{
    uint8_t *out = (uint8_t *)buffer;
    size_t done = 0U;

    if (done_out != NULL) {
        *done_out = 0U;
    }
    if (pipe == NULL || buffer == NULL || done_out == NULL || !pipe->used) {
        return PIPE_NULL;
    }
    if (pipe->count == 0U) {
        /* No writer left is EOF, not an error. */
        return pipe->writers == 0U ? PIPE_OK : PIPE_WOULD_BLOCK;
    }
    while (done < size && pipe->count != 0U) {
        out[done++] = pipe->data[pipe->head];
        pipe->head = (pipe->head + 1U) % HFOC_PIPE_CAPACITY;
        --pipe->count;
    }
    *done_out = done;
    pipe_wake_writers(pipe);
    return PIPE_OK;
}

enum pipe_status pipe_write_now(struct hfoc_pipe *pipe, const void *buffer,
                                size_t size, size_t *done_out)
{
    const uint8_t *in = (const uint8_t *)buffer;
    size_t done = 0U;

    if (done_out != NULL) {
        *done_out = 0U;
    }
    if (pipe == NULL || buffer == NULL || done_out == NULL || !pipe->used) {
        return PIPE_NULL;
    }
    if (pipe->writers == 0U) {
        return PIPE_BAD_STATE;
    }
    if (pipe->readers == 0U) {
        return PIPE_BROKEN;
    }
    if (pipe->count == HFOC_PIPE_CAPACITY) {
        return PIPE_WOULD_BLOCK;
    }
    while (done < size && pipe->count < HFOC_PIPE_CAPACITY) {
        const uint32_t tail =
            (pipe->head + pipe->count) % HFOC_PIPE_CAPACITY;

        pipe->data[tail] = in[done++];
        ++pipe->count;
    }
    *done_out = done;
    pipe_wake_readers(pipe);
    return PIPE_OK;
}

void pipe_end_retain(struct hfoc_pipe_end *end)
{
    if (end != NULL && end->pipe != NULL) {
        ++end->refs;
    }
}

void pipe_end_release(struct hfoc_pipe_end *end)
{
    struct hfoc_pipe *pipe;

    if (end == NULL || end->pipe == NULL || end->refs == 0U) {
        return;
    }
    --end->refs;
    if (end->refs != 0U) {
        return;
    }
    pipe = end->pipe;
    if (end->writer) {
        if (pipe->writers != 0U) {
            --pipe->writers;
        }
    } else {
        if (pipe->readers != 0U) {
            --pipe->readers;
        }
    }
    /* Closing one direction can turn a blocked peer into EOF or EPIPE. */
    pipe_wake_readers(pipe);
    pipe_wake_writers(pipe);
    if (pipe->readers == 0U && pipe->writers == 0U) {
        memset(pipe, 0, sizeof(*pipe));
    }
}

#ifndef HFOC_PIPE_HOST_TEST
static enum vfs_status pipe_vfs_status(enum pipe_status status)
{
    switch (status) {
    case PIPE_OK:
        return VFS_OK;
    case PIPE_BROKEN:
        return VFS_BROKEN_PIPE;
    case PIPE_WOULD_BLOCK:
        return VFS_BUSY;
    case PIPE_NO_SPACE:
        return VFS_NO_SPACE;
    case PIPE_NULL:
    case PIPE_BAD_STATE:
    case PIPE_NO_DESCRIPTOR:
    default:
        return VFS_BAD_FILE;
    }
}

static enum vfs_status pipe_descriptor_read(
    struct file_description *description, void *buffer, size_t size,
    size_t *done_out)
{
    struct hfoc_pipe_end *end =
        (struct hfoc_pipe_end *)description->context;

    if (end == NULL || end->pipe == NULL || end->writer) {
        return VFS_BAD_FILE;
    }
    for (;;) {
        size_t done = 0U;
        const enum pipe_status status =
            pipe_read_now(end->pipe, buffer, size, &done);

        if (status != PIPE_WOULD_BLOCK) {
            if (status == PIPE_OK) {
                *done_out = done;
                return VFS_OK;
            }
            return pipe_vfs_status(status);
        }
        if (sched_wait(&end->pipe->read_wait, HFOC_PIPE_WAIT_TICKS) !=
            SCHEDULER_OK) {
            return VFS_BUSY;
        }
    }
}

static enum vfs_status pipe_descriptor_write(
    struct file_description *description, const void *buffer, size_t size,
    size_t *done_out)
{
    struct hfoc_pipe_end *end =
        (struct hfoc_pipe_end *)description->context;

    if (end == NULL || end->pipe == NULL || !end->writer) {
        return VFS_BAD_FILE;
    }
    for (;;) {
        size_t done = 0U;
        const enum pipe_status status =
            pipe_write_now(end->pipe, buffer, size, &done);

        if (status != PIPE_WOULD_BLOCK) {
            if (status == PIPE_OK) {
                *done_out = done;
                return VFS_OK;
            }
            return pipe_vfs_status(status);
        }
        if (sched_wait(&end->pipe->write_wait, HFOC_PIPE_WAIT_TICKS) !=
            SCHEDULER_OK) {
            return VFS_BUSY;
        }
    }
}

static void pipe_descriptor_close(struct file_description *description)
{
    if (description != NULL && description->context != NULL) {
        pipe_end_release((struct hfoc_pipe_end *)description->context);
        description->context = NULL;
    }
}

static const struct descriptor_ops pipe_descriptor_ops = {
    .read = pipe_descriptor_read,
    .write = pipe_descriptor_write,
    .close = pipe_descriptor_close,
};

enum pipe_status pipe_bind_fd(struct kernel_process *process,
                              struct hfoc_pipe *pipe, bool writer,
                              uint32_t *descriptor_out)
{
    struct hfoc_pipe_end *end;
    struct file_description *storage = NULL;
    enum descriptor_status status;
    uint32_t index;

    if (process == NULL || pipe == NULL || descriptor_out == NULL ||
        !pipe->used) {
        return PIPE_NULL;
    }
    *descriptor_out = 0U;
    for (index = 0U; index < PROCESS_DESCRIPTOR_CAPACITY; ++index) {
        if (!process->file_descriptions[index].published) {
            storage = &process->file_descriptions[index];
            break;
        }
    }
    if (storage == NULL) {
        return PIPE_NO_DESCRIPTOR;
    }
    end = writer ? &pipe->write_end : &pipe->read_end;
    pipe_end_retain(end);
    if (descriptor_description_init(storage, &pipe_descriptor_ops, end,
                                    !writer, writer) != DESCRIPTOR_OK) {
        pipe_end_release(end);
        return PIPE_BAD_STATE;
    }
    status = descriptor_table_bind(&process->descriptor_table, storage,
                                   descriptor_out);
    if (status != DESCRIPTOR_OK) {
        descriptor_description_release(storage);
        return PIPE_NO_DESCRIPTOR;
    }
    return PIPE_OK;
}

bool pipe_description_is_pipe(const struct file_description *description)
{
    return description != NULL && description->published &&
           description->ops == &pipe_descriptor_ops &&
           description->context != NULL;
}

enum pipe_status pipe_adopt_fd(struct kernel_process *process,
                               const struct file_description *source,
                               uint32_t descriptor)
{
    struct hfoc_pipe_end *end;
    struct file_description *storage;
    enum descriptor_status status;

    if (process == NULL || !pipe_description_is_pipe(source)) {
        return PIPE_NULL;
    }
    if (descriptor >= 3U) {
        /* ITEM-628 applies redirections only to the three standard streams. */
        return PIPE_BAD_STATE;
    }
    storage = &process->stdio[descriptor];
    if (descriptor_table_lookup(&process->descriptor_table, descriptor,
                                &storage) == DESCRIPTOR_OK) {
        if (descriptor_table_close(&process->descriptor_table,
                                   descriptor) != DESCRIPTOR_OK) {
            return PIPE_BAD_STATE;
        }
    }
    storage = &process->stdio[descriptor];
    end = (struct hfoc_pipe_end *)source->context;
    pipe_end_retain(end);
    if (descriptor_description_init(storage, &pipe_descriptor_ops, end,
                                    !end->writer, end->writer) !=
        DESCRIPTOR_OK) {
        pipe_end_release(end);
        return PIPE_BAD_STATE;
    }
    status = descriptor_table_bind_at(&process->descriptor_table, storage,
                                      descriptor);
    if (status != DESCRIPTOR_OK) {
        descriptor_description_release(storage);
        return PIPE_NO_DESCRIPTOR;
    }
    return PIPE_OK;
}
#endif
