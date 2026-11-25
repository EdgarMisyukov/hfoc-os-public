/*
 * ITEM-629: RFC 4254 pty-req/shell/window-change/signal, channel input/output
 * and close, driven through the production transport state machine.
 */
#include "hfoc_ssh_transport.h"
#include "hfoc_ssh_wire.h"
#include "tty.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(condition, ...)                                                 \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL: ");                                        \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

static int host_rng(void *context, unsigned char *output, size_t length)
{
    int fd = *(int *)context;
    size_t done = 0U;

    while (done < length) {
        ssize_t got = read(fd, output + done, length - done);

        if (got <= 0) {
            return -1;
        }
        done += (size_t)got;
    }
    return 0;
}

static int command_stub(void *context, const char *command, uint8_t *out,
                        size_t capacity, size_t *out_length)
{
    (void)context;
    (void)command;
    (void)capacity;
    out[0] = '\0';
    *out_length = 0U;
    return 0;
}

struct shell_context {
    char term[80];
    uint32_t columns;
    uint32_t rows;
    uint32_t starts;
    uint32_t inputs;
    uint32_t resizes;
    uint32_t signals;
    uint32_t eofs;
    uint32_t hangups;
    bool exited;
    int exit_status;
    uint8_t input[4096];
    size_t input_length;
    uint8_t output[4096];
    size_t output_offset;
    size_t output_length;
};

static bool shell_start(void *context, const char *term, uint32_t columns,
                        uint32_t rows)
{
    struct shell_context *shell = context;

    ++shell->starts;
    memset(shell->term, 0, sizeof(shell->term));
    if (term != NULL) {
        strncpy(shell->term, term, sizeof(shell->term) - 1U);
    }
    shell->columns = columns;
    shell->rows = rows;
    memcpy(shell->output, "hfoc$ ", 6U);
    shell->output_offset = 0U;
    shell->output_length = 6U;
    return true;
}

static bool shell_input(void *context, const uint8_t *data, size_t length,
                        size_t *accepted)
{
    struct shell_context *shell = context;

    ++shell->inputs;
    if (length > sizeof(shell->input) - shell->input_length) {
        *accepted = 0U;
        return false;
    }
    memcpy(shell->input + shell->input_length, data, length);
    shell->input_length += length;
    *accepted = length;
    return true;
}

static uint32_t shell_input_space(void *context)
{
    (void)context;
    return HFOC_TTY_INPUT_MAX;
}

static bool shell_resize(void *context, uint32_t columns, uint32_t rows)
{
    struct shell_context *shell = context;

    ++shell->resizes;
    shell->columns = columns;
    shell->rows = rows;
    return true;
}

static bool shell_signal(void *context, const char *name, size_t length)
{
    struct shell_context *shell = context;

    ++shell->signals;
    return length == 3U && memcmp(name, "INT", 3U) == 0;
}

static size_t shell_output(void *context, uint8_t *buffer, size_t capacity)
{
    struct shell_context *shell = context;
    size_t available = shell->output_length - shell->output_offset;
    size_t take = available < capacity ? available : capacity;

    if (take != 0U) {
        memcpy(buffer, shell->output + shell->output_offset, take);
        shell->output_offset += take;
    }
    return take;
}

static bool shell_exit_status(void *context, int *status)
{
    struct shell_context *shell = context;

    if (!shell->exited) {
        return false;
    }
    *status = shell->exit_status;
    return true;
}

static void shell_eof(void *context)
{
    struct shell_context *shell = context;

    ++shell->eofs;
}

static void shell_hangup(void *context)
{
    struct shell_context *shell = context;

    ++shell->hangups;
}

static const struct hfoc_ssh_shell_ops shell_ops = {
    .start = shell_start,
    .input = shell_input,
    .input_space = shell_input_space,
    .resize = shell_resize,
    .signal = shell_signal,
    .output = shell_output,
    .exit_status = shell_exit_status,
    .eof = shell_eof,
    .hangup = shell_hangup,
};

static size_t packet(uint8_t *out, const uint8_t *payload, size_t length)
{
    size_t padding = 8U - ((4U + 1U + length) % 8U);

    if (padding < 4U) {
        padding += 8U;
    }
    out[0] = (uint8_t)((1U + length + padding) >> 24);
    out[1] = (uint8_t)((1U + length + padding) >> 16);
    out[2] = (uint8_t)((1U + length + padding) >> 8);
    out[3] = (uint8_t)(1U + length + padding);
    out[4] = (uint8_t)padding;
    memcpy(out + 5U, payload, length);
    memset(out + 5U + length, 0, padding);
    return 4U + 1U + length + padding;
}

static size_t request_begin(uint8_t *payload, const char *name, bool want_reply)
{
    struct hfoc_ssh_writer writer;

    hfoc_ssh_writer_init(&writer, payload, 256U);
    (void)hfoc_ssh_write_u8(&writer, 98U);
    (void)hfoc_ssh_write_u32(&writer, 0U);
    (void)hfoc_ssh_write_cstring(&writer, name);
    (void)hfoc_ssh_write_bool(&writer, want_reply);
    return writer.length;
}

static size_t request_pty(uint8_t *payload, size_t capacity, const char *term,
                          uint32_t columns, uint32_t rows,
                          const uint8_t *modes, size_t modes_length)
{
    struct hfoc_ssh_writer writer;
    const size_t at = request_begin(payload, "pty-req", true);

    hfoc_ssh_writer_init(&writer, payload + at, capacity - at);
    (void)hfoc_ssh_write_cstring(&writer, term);
    (void)hfoc_ssh_write_u32(&writer, columns);
    (void)hfoc_ssh_write_u32(&writer, rows);
    (void)hfoc_ssh_write_u32(&writer, 0U);
    (void)hfoc_ssh_write_u32(&writer, 0U);
    (void)hfoc_ssh_write_string(&writer, modes, modes_length);
    return at + writer.length;
}

static size_t request_shell(uint8_t *payload, bool want_reply)
{
    return request_begin(payload, "shell", want_reply);
}

static size_t request_window(uint8_t *payload, uint32_t columns,
                             uint32_t rows)
{
    struct hfoc_ssh_writer writer;
    size_t at = request_begin(payload, "window-change", false);

    hfoc_ssh_writer_init(&writer, payload + at, 256U - at);
    (void)hfoc_ssh_write_u32(&writer, columns);
    (void)hfoc_ssh_write_u32(&writer, rows);
    (void)hfoc_ssh_write_u32(&writer, 0U);
    (void)hfoc_ssh_write_u32(&writer, 0U);
    return at + writer.length;
}

static size_t request_signal(uint8_t *payload, const char *name)
{
    struct hfoc_ssh_writer writer;
    size_t at = request_begin(payload, "signal", false);

    hfoc_ssh_writer_init(&writer, payload + at, 256U - at);
    (void)hfoc_ssh_write_cstring(&writer, name);
    return at + writer.length;
}

static size_t channel_data(uint8_t *payload, const char *text)
{
    struct hfoc_ssh_writer writer;

    hfoc_ssh_writer_init(&writer, payload, 256U);
    (void)hfoc_ssh_write_u8(&writer, 94U);
    (void)hfoc_ssh_write_u32(&writer, 0U);
    (void)hfoc_ssh_write_cstring(&writer, text);
    return writer.length;
}

static size_t channel_eof(uint8_t *payload)
{
    struct hfoc_ssh_writer writer;

    hfoc_ssh_writer_init(&writer, payload, 16U);
    (void)hfoc_ssh_write_u8(&writer, 96U);
    (void)hfoc_ssh_write_u32(&writer, 0U);
    return writer.length;
}

static enum hfoc_ssh_session_result feed(struct hfoc_ssh_session *session,
                                         const uint8_t *payload,
                                         size_t length)
{
    uint8_t wire[1024];
    const size_t wire_length = packet(wire, payload, length);

    return hfoc_ssh_session_feed(session, wire, wire_length, 1U);
}

static bool has_message(const uint8_t *data, size_t length, uint8_t message)
{
    size_t at = 0U;

    while (at + 5U <= length) {
        const uint32_t packet_length =
            ((uint32_t)data[at] << 24) | ((uint32_t)data[at + 1U] << 16) |
            ((uint32_t)data[at + 2U] << 8) | data[at + 3U];
        const size_t end = at + 4U + packet_length;

        if (end > length || packet_length < 5U) {
            return false;
        }
        if (data[at + 5U] == message) {
            return true;
        }
        at = end;
    }
    return false;
}

int main(void)
{
    static struct hfoc_ssh_session session;
    static struct shell_context shell;
    uint8_t payload[1024];
    uint8_t modes[159U * 5U + 2U] = { 0U };
    uint8_t wire[HFOC_SSH_SESSION_OUTPUT_MAX];
    int urandom_fd;
    size_t length;
    size_t taken;

    urandom_fd = open("/dev/urandom", 0);
    CHECK(urandom_fd >= 0, "cannot open /dev/urandom");
    if (urandom_fd < 0) {
        return 1;
    }
    memset(&shell, 0, sizeof(shell));
    CHECK(hfoc_ssh_session_init(&session, host_rng, &urandom_fd, command_stub,
                                NULL),
          "session init failed");
    hfoc_ssh_session_set_shell(&session, &shell_ops, &shell);
    session.state = HFOC_SSH_SESSION_STATE_CHANNEL;
    session.channel_open = true;
    session.client_channel = 7U;
    session.server_channel = 0U;
    session.client_window = 65536U;
    session.client_max_packet = 16384U;
    session.output_length = 0U;

    for (size_t index = 0U; index < 159U; ++index) {
        modes[index * 5U] = (uint8_t)(index + 1U);
    }
    length = request_pty(payload, sizeof(payload), "xterm", 20U, 4U,
                         modes, sizeof(modes));
    CHECK(feed(&session, payload, length) == HFOC_SSH_SESSION_NEED_MORE,
          "overlong pty mode string keeps the channel open");
    taken = hfoc_ssh_session_take_output(&session, wire, sizeof(wire));
    CHECK(!session.pty_requested && has_message(wire, taken, 100U),
          "pty mode length stays bounded");

    length = request_pty(payload, sizeof(payload), "xterm", 20U, 4U,
                         modes, sizeof(modes) - 3U);
    CHECK(feed(&session, payload, length) == HFOC_SSH_SESSION_NEED_MORE,
          "truncated pty mode argument keeps the channel open");
    taken = hfoc_ssh_session_take_output(&session, wire, sizeof(wire));
    CHECK(!session.pty_requested && has_message(wire, taken, 100U),
          "truncated pty mode argument is refused");

    length = request_pty(payload, sizeof(payload), "xterm-256color", 100U, 40U,
                         modes, sizeof(modes) - 1U);
    CHECK(feed(&session, payload, length) == HFOC_SSH_SESSION_NEED_MORE,
          "pty-req does not end the session");
    CHECK(session.pty_requested && strcmp(session.pty_term, "xterm-256color") == 0,
          "pty-req stores the terminal name");
    CHECK(session.pty_columns == 100U && session.pty_rows == 40U,
          "pty-req stores the geometry");
    taken = hfoc_ssh_session_take_output(&session, wire, sizeof(wire));
    CHECK(has_message(wire, taken, 99U), "full pty mode string is acknowledged");

    length = request_shell(payload, true);
    CHECK(feed(&session, payload, length) == HFOC_SSH_SESSION_NEED_MORE,
          "shell does not end the session");
    CHECK(shell.starts == 1U && strcmp(shell.term, "xterm-256color") == 0,
          "shell starts with the pty terminal");
    CHECK(shell.columns == 100U && shell.rows == 40U,
          "shell starts with the pty geometry");
    CHECK(hfoc_ssh_session_flush_output(&session), "shell prompt flush succeeds");
    taken = hfoc_ssh_session_take_output(&session, wire, sizeof(wire));
    CHECK(has_message(wire, taken, 99U), "shell is acknowledged");
    CHECK(has_message(wire, taken, 94U), "the shell prompt is channel data");

    length = channel_data(payload, "echo hi\n");
    CHECK(feed(&session, payload, length) == HFOC_SSH_SESSION_NEED_MORE,
          "channel data is accepted");
    CHECK(shell.inputs == 1U && shell.input_length == 8U &&
              memcmp(shell.input, "echo hi\n", 8U) == 0,
          "channel data reaches the session TTY");

    length = request_window(payload, 120U, 50U);
    CHECK(feed(&session, payload, length) == HFOC_SSH_SESSION_NEED_MORE,
          "window-change is accepted");
    CHECK(shell.resizes == 1U && shell.columns == 120U && shell.rows == 50U,
          "window-change reaches the TTY owner");

    length = request_signal(payload, "INT");
    CHECK(feed(&session, payload, length) == HFOC_SSH_SESSION_NEED_MORE,
          "signal is accepted");
    CHECK(shell.signals == 1U, "signal reaches the session owner");

    length = channel_eof(payload);
    CHECK(feed(&session, payload, length) == HFOC_SSH_SESSION_NEED_MORE,
          "EOF is accepted");
    CHECK(shell.eofs == 1U && session.shell_eof,
          "EOF reaches the session input");

    shell.exited = true;
    shell.exit_status = 7;
    CHECK(hfoc_ssh_session_flush_output(&session),
          "exit status flush succeeds");
    taken = hfoc_ssh_session_take_output(&session, wire, sizeof(wire));
    CHECK(has_message(wire, taken, 98U), "exit-status is queued");
    CHECK(has_message(wire, taken, 96U), "EOF is queued");
    CHECK(has_message(wire, taken, 97U), "close is queued");

    close(urandom_fd);
    if (failures != 0) {
        fprintf(stderr, "ssh shell host test: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("ssh shell host test: ALL OK\n");
    return 0;
}
