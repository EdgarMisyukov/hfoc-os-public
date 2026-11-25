/*
 * ITEM-602: the transport's bounded command-output path.
 *
 * `hfoc diag` is larger than one channel window and can be larger than one
 * 8 KiB adapter buffer.  This proof drives the session state machine directly:
 * a small peer window must defer the rest (not fail the command), draining the
 * transport output must let the next slice through, and the exit-status/EOF/
 * close frame must be queued only after the last data byte.
 */
#include "hfoc_ssh_transport.h"

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

struct drain {
    size_t data_bytes;
    size_t data_packets;
    size_t max_data_packet;
    bool exit_status;
    bool eof;
    bool close;
    bool malformed;
    bool data_after_eof;
    bool close_before_eof;
};

static uint32_t be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint32_t be32_at(const uint8_t *bytes, size_t at)
{
    return ((uint32_t)bytes[at] << 24) | ((uint32_t)bytes[at + 1U] << 16) |
           ((uint32_t)bytes[at + 2U] << 8) | (uint32_t)bytes[at + 3U];
}

/*
 * The session is still pre-NEWKEYS in this proof, so the packet framing is the
 * plaintext form: length, padding_length, payload, padding.  Decode the
 * channel messages the transport produced and count the payload bytes.
 */
static void drain_packets(const uint8_t *packets, size_t length,
                          struct drain *drain)
{
    size_t at = 0U;

    while (at + 5U <= length) {
        const uint32_t packet_length = be32(packets + at);
        const size_t packet_end = at + 4U + packet_length;
        const uint8_t padding = packets[at + 4U];
        const size_t payload_start = at + 5U;
        const size_t payload_length =
            packet_length >= (uint32_t)padding + 1U
                ? packet_length - padding - 1U
                : 0U;

        if (packet_end > length || payload_length == 0U) {
            drain->malformed = true;
            return;
        }
        switch (packets[payload_start]) {
        case 94U: /* CHANNEL_DATA */
            if (payload_length >= 9U) {
                const uint32_t string_length =
                    be32_at(packets, payload_start + 5U);
                if (9U + string_length <= payload_length) {
                    if (drain->eof || drain->close) {
                        drain->data_after_eof = true;
                    }
                    drain->data_bytes += string_length;
                    ++drain->data_packets;
                    if (string_length > drain->max_data_packet) {
                        drain->max_data_packet = string_length;
                    }
                } else {
                    drain->malformed = true;
                    return;
                }
            } else {
                drain->malformed = true;
                return;
            }
            break;
        case 96U: /* CHANNEL_EOF */
            drain->eof = true;
            break;
        case 97U: /* CHANNEL_CLOSE */
            if (!drain->eof) {
                drain->close_before_eof = true;
            }
            drain->close = true;
            break;
        case 98U: /* CHANNEL_REQUEST: exit-status */
            drain->exit_status = true;
            break;
        default:
            break;
        }
        at = packet_end;
    }
}

int main(void)
{
    static struct hfoc_ssh_session session;
    static uint8_t wire[HFOC_SSH_SESSION_OUTPUT_MAX];
    struct drain drain;
    int urandom_fd;
    size_t total = 0U;
    size_t expected;
    bool progressed;

    urandom_fd = open("/dev/urandom", 0);
    CHECK(urandom_fd >= 0, "cannot open /dev/urandom");
    if (urandom_fd < 0) {
        return 1;
    }
    CHECK(hfoc_ssh_session_init(&session, host_rng, &urandom_fd, command_stub,
                                NULL),
          "session init failed");
    session.output_length = 0U; /* drop the server identification banner */
    session.state = HFOC_SSH_SESSION_STATE_CHANNEL;
    session.channel_open = true;
    session.client_channel = 7U;
    session.client_window = 64U; /* deliberately tiny */
    session.client_max_packet = 1000U;
    session.exec_started = true;
    session.exec_length = 20000U;
    session.exec_offset = 0U;
    session.exec_status = 0;
    session.exit_sent = false;
    for (size_t index = 0U; index < session.exec_length; ++index) {
        session.exec_output[index] = (uint8_t)('a' + (index % 26U));
    }

    memset(&drain, 0, sizeof(drain));
    /*
     * First pass: the tiny window admits one 64-byte chunk, then the flush
     * must wait for a window adjust instead of disconnecting.
     */
    CHECK(hfoc_ssh_session_flush_output(&session),
          "a small window failed the command");
    CHECK(session.client_window == 0U, "the window was not consumed");
    CHECK(session.exec_offset == 64U, "wrote %zu bytes, want 64",
          session.exec_offset);
    {
        const size_t taken = hfoc_ssh_session_take_output(
            &session, wire, sizeof(wire));
        drain_packets(wire, taken, &drain);
        total += drain.data_bytes;
    }
    CHECK(drain.data_bytes == 64U, "first pass carried %zu data bytes",
          drain.data_bytes);

    /*
     * Simulate the client's WINDOW_ADJUST and the adapter pump: each turn
     * opens the window, flushes a slice, then drains it.  Every byte must
     * arrive, and the close frame must be last.
     */
    expected = session.exec_length;
    do {
        progressed = false;
        session.client_window = 4096U;
        CHECK(hfoc_ssh_session_flush_output(&session),
              "flush failed with an open window at %zu/%zu",
              session.exec_offset, session.exec_length);
        for (;;) {
            const size_t taken = hfoc_ssh_session_take_output(
                &session, wire, sizeof(wire));
            struct drain before = drain;

            if (taken == 0U) {
                break;
            }
            drain_packets(wire, taken, &drain);
            total += drain.data_bytes - before.data_bytes;
            progressed = true;
        }
        CHECK(!drain.malformed, "the transport produced a malformed packet");
    } while (session.exec_offset < session.exec_length &&
             session.client_window == 0U && progressed);

    CHECK(session.exec_offset == expected,
          "only %zu of %zu bytes were handed to the channel",
          session.exec_offset, expected);
    CHECK(total == expected, "the channel carried %zu, want %zu", total,
          expected);
    CHECK(drain.max_data_packet <= 1000U,
          "a CHANNEL_DATA packet carried %zu bytes, peer max is 1000",
          drain.max_data_packet);
    CHECK(drain.close, "the channel never closed after the command");
    CHECK(drain.eof, "the channel never sent EOF");
    CHECK(drain.exit_status, "the channel never sent exit-status");
    CHECK(!drain.data_after_eof,
          "CHANNEL_DATA arrived after the channel EOF");
    CHECK(!drain.close_before_eof,
          "CHANNEL_CLOSE arrived before CHANNEL_EOF");
    CHECK(session.state == HFOC_SSH_SESSION_STATE_CLOSING,
          "the session state is %d, want CLOSING", (int)session.state);

    hfoc_ssh_session_free(&session);
    close(urandom_fd);
    if (failures != 0) {
        fprintf(stderr, "ssh transport window host: %d failure(s)\n", failures);
        return 1;
    }
    printf("ssh transport window host: ALL OK\n");
    return 0;
}
