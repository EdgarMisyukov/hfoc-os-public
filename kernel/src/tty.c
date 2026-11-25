/*
 * ITEM-629: bounded terminal queues over the existing scheduler wait queues.
 * No interrupt or timer ownership is added here.
 */
#include "tty.h"

#include "descriptor.h"

#ifndef HFOC_TTY_HOST_TEST
#include "process.h"
#include "scheduler.h"
#endif

#include <string.h>

struct tty {
    bool used;
    uint32_t refs;
    bool hung_up;
    bool input_eof;
    uint32_t foreground_pgid;
    uint32_t columns;
    uint32_t rows;
    uint8_t input[HFOC_TTY_INPUT_MAX];
    uint32_t input_head;
    uint32_t input_count;
    uint8_t output[HFOC_TTY_OUTPUT_MAX];
    uint32_t output_head;
    uint32_t output_count;
    struct scheduler_wait_queue read_wait;
    struct scheduler_wait_queue write_wait;
};

static struct tty ttys[HFOC_TTY_MAX];

static void tty_wait_init(struct scheduler_wait_queue *queue)
{
#ifdef HFOC_TTY_HOST_TEST
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0U;
#else
    (void)sched_wait_queue_init(queue);
#endif
}

static void tty_wake_readers(struct tty *tty)
{
#ifndef HFOC_TTY_HOST_TEST
    (void)sched_wake_all(&tty->read_wait);
#else
    (void)tty;
#endif
}

static void tty_wake_writers(struct tty *tty)
{
#ifndef HFOC_TTY_HOST_TEST
    (void)sched_wake_all(&tty->write_wait);
#else
    (void)tty;
#endif
}

static struct tty *tty_alloc(void)
{
    uint32_t index;

    for (index = 0U; index < HFOC_TTY_MAX; ++index) {
        if (!ttys[index].used) {
            return &ttys[index];
        }
    }
    return NULL;
}

static void tty_release_ref(struct tty *tty, bool hang_up)
{
    if (tty == NULL || !tty->used || tty->refs == 0U) {
        return;
    }
    if (hang_up) {
        tty_hangup(tty);
    }
    --tty->refs;
    if (tty->refs == 0U) {
        memset(tty, 0, sizeof(*tty));
    }
}

enum tty_status tty_create(struct tty **tty_out)
{
    struct tty *tty;

    if (tty_out == NULL) {
        return TTY_NULL;
    }
    *tty_out = NULL;
    tty = tty_alloc();
    if (tty == NULL) {
        return TTY_NO_SPACE;
    }
    memset(tty, 0, sizeof(*tty));
    tty->used = true;
    tty->refs = 1U;
    tty->columns = 80U;
    tty->rows = 24U;
    tty_wait_init(&tty->read_wait);
    tty_wait_init(&tty->write_wait);
    *tty_out = tty;
    return TTY_OK;
}

void tty_destroy(struct tty *tty)
{
    tty_release_ref(tty, true);
}

void tty_hangup(struct tty *tty)
{
    if (tty == NULL || !tty->used) {
        return;
    }
    tty->hung_up = true;
    tty->input_eof = true;
    tty_wake_readers(tty);
    tty_wake_writers(tty);
}

bool tty_hung_up(const struct tty *tty)
{
    return tty == NULL || !tty->used || tty->hung_up;
}

enum tty_status tty_input(struct tty *tty, const void *bytes, size_t length,
                          size_t *accepted_out)
{
    const uint8_t *data = (const uint8_t *)bytes;
    size_t accepted = 0U;

    if (accepted_out != NULL) {
        *accepted_out = 0U;
    }
    if (tty == NULL || bytes == NULL || accepted_out == NULL || !tty->used) {
        return TTY_NULL;
    }
    if (tty->hung_up || tty->input_eof) {
        return TTY_HUNGUP;
    }
    while (accepted < length && tty->input_count < HFOC_TTY_INPUT_MAX) {
        const uint32_t tail =
            (tty->input_head + tty->input_count) % HFOC_TTY_INPUT_MAX;
        const uint8_t byte = data[accepted];

        /*
         * ITEM-633: while a foreground job owns the terminal, Ctrl-C,
         * Ctrl-\ and Ctrl-Z become signals for that process group instead of
         * bytes the background reader could consume.
         */
        if (tty->foreground_pgid != 0U &&
            (byte == 0x03U || byte == 0x1cU || byte == 0x1aU)) {
#ifndef HFOC_TTY_HOST_TEST
            const uint32_t signal = byte == 0x03U ? PROCESS_SIGNAL_INT
                                   : byte == 0x1cU ? PROCESS_SIGNAL_QUIT
                                                   : PROCESS_SIGNAL_TSTP;

            (void)process_signal_group(tty->foreground_pgid, signal);
#endif
            ++accepted;
            continue;
        }
        tty->input[tail] = byte;
        ++accepted;
        ++tty->input_count;
    }
    *accepted_out = accepted;
    if (accepted != 0U) {
        tty_wake_readers(tty);
    }
    return accepted == length ? TTY_OK : TTY_FULL;
}

void tty_input_eof(struct tty *tty)
{
    if (tty == NULL || !tty->used) {
        return;
    }
    tty->input_eof = true;
    tty_wake_readers(tty);
}

enum tty_status tty_read_now(struct tty *tty, void *buffer, size_t length,
                             size_t *done_out)
{
    uint8_t *out = (uint8_t *)buffer;
    size_t done = 0U;

    if (done_out != NULL) {
        *done_out = 0U;
    }
    if (tty == NULL || buffer == NULL || done_out == NULL || !tty->used) {
        return TTY_NULL;
    }
    while (done < length && tty->input_count != 0U) {
        out[done++] = tty->input[tty->input_head];
        tty->input_head = (tty->input_head + 1U) % HFOC_TTY_INPUT_MAX;
        --tty->input_count;
    }
    *done_out = done;
    if (done != 0U) {
        tty_wake_writers(tty);
        return TTY_OK;
    }
    if (tty->input_eof || tty->hung_up) {
        return TTY_OK; /* EOF, not an error */
    }
    return TTY_AGAIN;
}

enum tty_status tty_write_now(struct tty *tty, const void *bytes,
                              size_t length, size_t *done_out)
{
    const uint8_t *data = (const uint8_t *)bytes;
    size_t done = 0U;

    if (done_out != NULL) {
        *done_out = 0U;
    }
    if (tty == NULL || bytes == NULL || done_out == NULL || !tty->used) {
        return TTY_NULL;
    }
    if (tty->hung_up) {
        return TTY_HUNGUP;
    }
    while (done < length && tty->output_count < HFOC_TTY_OUTPUT_MAX) {
        const uint32_t tail =
            (tty->output_head + tty->output_count) % HFOC_TTY_OUTPUT_MAX;

        tty->output[tail] = data[done++];
        ++tty->output_count;
    }
    *done_out = done;
    if (done != 0U) {
        tty_wake_writers(tty);
        return TTY_OK;
    }
    return length == 0U ? TTY_OK : TTY_AGAIN;
}

enum tty_status tty_output_take(struct tty *tty, void *buffer, size_t capacity,
                                size_t *done_out)
{
    uint8_t *out = (uint8_t *)buffer;
    size_t done = 0U;

    if (done_out != NULL) {
        *done_out = 0U;
    }
    if (tty == NULL || buffer == NULL || done_out == NULL || !tty->used) {
        return TTY_NULL;
    }
    while (done < capacity && tty->output_count != 0U) {
        out[done++] = tty->output[tty->output_head];
        tty->output_head = (tty->output_head + 1U) % HFOC_TTY_OUTPUT_MAX;
        --tty->output_count;
    }
    *done_out = done;
    if (done != 0U) {
        tty_wake_writers(tty);
    }
    return TTY_OK;
}

uint32_t tty_input_pending(const struct tty *tty)
{
    return tty != NULL && tty->used ? tty->input_count : 0U;
}

uint32_t tty_input_free(const struct tty *tty)
{
    return tty != NULL && tty->used ? HFOC_TTY_INPUT_MAX - tty->input_count : 0U;
}

uint32_t tty_output_pending(const struct tty *tty)
{
    return tty != NULL && tty->used ? tty->output_count : 0U;
}

void tty_set_foreground(struct tty *tty, uint32_t pgid)
{
    if (tty != NULL && tty->used) {
        tty->foreground_pgid = pgid;
    }
}

uint32_t tty_foreground(const struct tty *tty)
{
    return tty != NULL && tty->used ? tty->foreground_pgid : 0U;
}

void tty_resize(struct tty *tty, uint32_t columns, uint32_t rows)
{
    if (tty == NULL || !tty->used) {
        return;
    }
    tty->columns = columns != 0U && columns <= 4096U ? columns : 80U;
    tty->rows = rows != 0U && rows <= 4096U ? rows : 24U;
}

uint32_t tty_columns(const struct tty *tty)
{
    return tty != NULL && tty->used ? tty->columns : 0U;
}

uint32_t tty_rows(const struct tty *tty)
{
    return tty != NULL && tty->used ? tty->rows : 0U;
}

#ifndef HFOC_TTY_HOST_TEST

static enum vfs_status tty_vfs_status(enum tty_status status)
{
    switch (status) {
    case TTY_OK:
        return VFS_OK;
    case TTY_HUNGUP:
        return VFS_BROKEN_PIPE;
    case TTY_AGAIN:
    case TTY_FULL:
        return VFS_BUSY;
    case TTY_NULL:
    case TTY_NO_SPACE:
    case TTY_BAD_STATE:
    case TTY_NO_DESCRIPTOR:
    default:
        return VFS_BAD_FILE;
    }
}

static enum vfs_status tty_descriptor_read(
    struct file_description *description, void *buffer, size_t length,
    size_t *done_out)
{
    struct tty *tty = description != NULL ? description->context : NULL;

    if (tty == NULL || !tty->used) {
        return VFS_BAD_FILE;
    }
#ifndef HFOC_TTY_HOST_TEST
    if (tty->foreground_pgid != 0U) {
        struct scheduler_task *task = NULL;
        struct kernel_process *process = NULL;

        if (sched_current(&task) == SCHEDULER_OK && task != NULL) {
            process = (struct kernel_process *)task->process_owner;
        }
        if (process == NULL ||
            process_pgid(process) != tty->foreground_pgid) {
            return VFS_BAD_FILE; /* a background group cannot read the TTY */
        }
    }
#endif
    for (;;) {
        size_t done = 0U;
        const enum tty_status status = tty_read_now(tty, buffer, length, &done);

        if (status == TTY_OK) {
            *done_out = done;
            return VFS_OK;
        }
        if (status != TTY_AGAIN) {
            return tty_vfs_status(status);
        }
        if (sched_wait(&tty->read_wait, HFOC_TTY_WAIT_TICKS) !=
            SCHEDULER_OK) {
            return VFS_BUSY;
        }
    }
}

static enum vfs_status tty_descriptor_write(
    struct file_description *description, const void *buffer, size_t length,
    size_t *done_out)
{
    struct tty *tty = description != NULL ? description->context : NULL;
    size_t total = 0U;

    if (tty == NULL || !tty->used) {
        return VFS_BAD_FILE;
    }
    while (total < length) {
        size_t done = 0U;
        const enum tty_status status =
            tty_write_now(tty, (const uint8_t *)buffer + total,
                          length - total, &done);

        total += done;
        if (status == TTY_OK && total == length) {
            *done_out = total;
            return VFS_OK;
        }
        if (status == TTY_HUNGUP) {
            return total != 0U ? VFS_OK : VFS_BROKEN_PIPE;
        }
        if (status != TTY_AGAIN && status != TTY_OK) {
            return tty_vfs_status(status);
        }
        if (sched_wait(&tty->write_wait, HFOC_TTY_WAIT_TICKS) !=
            SCHEDULER_OK) {
            *done_out = total;
            return total != 0U ? VFS_OK : VFS_BUSY;
        }
    }
    *done_out = total;
    return VFS_OK;
}

static void tty_descriptor_close(struct file_description *description)
{
    if (description != NULL && description->context != NULL) {
        struct tty *tty = (struct tty *)description->context;

        description->context = NULL;
        tty_release_ref(tty, false);
    }
}

static const struct descriptor_ops tty_descriptor_ops = {
    .read = tty_descriptor_read,
    .write = tty_descriptor_write,
    .close = tty_descriptor_close,
};

bool tty_description_is_tty(const struct file_description *description)
{
    return description != NULL && description->published &&
           description->ops == &tty_descriptor_ops &&
           description->context != NULL;
}

enum tty_status tty_adopt_fd(struct kernel_process *process,
                             const struct tty *source, uint32_t descriptor)
{
    struct tty *tty = (struct tty *)(uintptr_t)source;
    struct file_description *storage;
    struct file_description *existing = NULL;
    enum descriptor_status status;

    if (process == NULL || tty == NULL || !tty->used) {
        return TTY_NULL;
    }
    if (descriptor >= 3U) {
        return TTY_BAD_STATE;
    }
    storage = &process->stdio[descriptor];
    if (descriptor_table_lookup(&process->descriptor_table, descriptor,
                                &existing) == DESCRIPTOR_OK) {
        if (descriptor_table_close(&process->descriptor_table,
                                   descriptor) != DESCRIPTOR_OK) {
            return TTY_BAD_STATE;
        }
    }
    storage = &process->stdio[descriptor];
    ++tty->refs;
    if (descriptor_description_init(
            storage, &tty_descriptor_ops, tty, descriptor == 0U,
            descriptor != 0U) != DESCRIPTOR_OK) {
        tty_release_ref(tty, false);
        return TTY_BAD_STATE;
    }
    status = descriptor_table_bind_at(&process->descriptor_table, storage,
                                      descriptor);
    if (status != DESCRIPTOR_OK) {
        descriptor_description_release(storage);
        return TTY_NO_DESCRIPTOR;
    }
    return TTY_OK;
}

#endif
