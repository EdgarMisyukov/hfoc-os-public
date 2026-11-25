/*
 * ITEM-603: adversarial input and lifecycle transitions over the real SSH
 * transport.
 *
 * The production adapter feeds arbitrary TCP bytes into
 * `hfoc_ssh_session_feed()`; this proof drives the same state machine with
 * fragmented notifications, malformed packet framing, pre-auth IGNORE floods
 * and the real tick deadlines.  It asserts named results and reasons, never a
 * crash or a silent success.
 */
#include "hfoc_ssh_transport.h"

#include "hfoc_ssh_wire.h"

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

static struct hfoc_ssh_session session;
static int urandom_fd = -1;

static int host_rng(void *context, unsigned char *output, size_t length)
{
    size_t done = 0U;

    (void)context;
    while (done < length) {
        ssize_t got = read(urandom_fd, output + done, length - done);

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

static void reset_session(void)
{
    hfoc_ssh_session_free(&session);
    CHECK(hfoc_ssh_session_init(&session, host_rng, NULL, command_stub, NULL),
          "session init failed");
    session.output_length = 0U;
    session.exec_started = false;
    session.exec_length = 0U;
    session.exec_offset = 0U;
    session.exit_sent = false;
}

static enum hfoc_ssh_session_result feed(const uint8_t *bytes, size_t length)
{
    return hfoc_ssh_session_feed(&session, bytes, length, 0U);
}

static void feed_identification(void)
{
    static const uint8_t ident[] = "SSH-2.0-fragment-test\r\n";

    CHECK(feed(ident, sizeof(ident) - 1U) == HFOC_SSH_SESSION_NEED_MORE,
          "identification was not accepted");
    CHECK(session.state >= HFOC_SSH_SESSION_STATE_KEXINIT,
          "the session did not leave IDENT (state=%d)", (int)session.state);
}

static void test_fragmented_identification(void)
{
    static const uint8_t ident[] = "SSH-2.0-one-byte\r\n";

    reset_session();
    for (size_t index = 0U; index < sizeof(ident) - 1U; ++index) {
        const enum hfoc_ssh_session_result result = feed(&ident[index], 1U);
        CHECK(result == HFOC_SSH_SESSION_NEED_MORE,
              "one-byte identification stopped at %zu with result %d", index,
              (int)result);
    }
    CHECK(session.state >= HFOC_SSH_SESSION_STATE_KEXINIT,
          "fragmented identification did not reach KEXINIT (state=%d)",
          (int)session.state);
}

static void test_identification_bounds(void)
{
    uint8_t line[HFOC_SSH_SESSION_VERSION_MAX + 32U];

    reset_session();
    memset(line, 'A', sizeof(line));
    CHECK(feed(line, sizeof(line)) == HFOC_SSH_SESSION_PROTOCOL_ERROR,
          "an overlong identification line was accepted");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "ident-too-long") == 0,
          "overlong identification reason=%s",
          hfoc_ssh_session_reason(&session));

    reset_session();
    {
        static const uint8_t preamble[] = "hello\r\nSSH-2.0-after-preamble\r\n";
        CHECK(feed(preamble, sizeof(preamble) - 1U) ==
                  HFOC_SSH_SESSION_NEED_MORE,
              "a bounded pre-identification line was not ignored");
        CHECK(session.state >= HFOC_SSH_SESSION_STATE_KEXINIT,
              "the preamble line broke the identification");
    }
}

static void test_malformed_packets(void)
{
    static const uint8_t zero_length[4] = {0U, 0U, 0U, 0U};
    static const uint8_t oversized[4] = {0U, 0U, 0x80U, 1U}; /* 32769 */
    static const uint8_t bad_padding[14] = {
        0U, 0U, 0U, 10U, 10U, 0U, 0U, 0U, 0U,
        0U, 0U, 0U, 0U, 0U,
    };
    static const uint8_t empty_payload[9] = {
        0U, 0U, 0U, 5U, 4U, 0U, 0U, 0U, 0U,
    };

    reset_session();
    feed_identification();
    CHECK(feed(zero_length, sizeof(zero_length)) ==
              HFOC_SSH_SESSION_PROTOCOL_ERROR,
          "a zero packet length was accepted");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "packet-length") == 0,
          "zero-length reason=%s", hfoc_ssh_session_reason(&session));

    reset_session();
    feed_identification();
    CHECK(feed(oversized, sizeof(oversized)) ==
              HFOC_SSH_SESSION_PROTOCOL_ERROR,
          "an oversized packet length was accepted");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "packet-length") == 0,
          "oversized reason=%s", hfoc_ssh_session_reason(&session));

    reset_session();
    feed_identification();
    CHECK(feed(bad_padding, sizeof(bad_padding)) ==
              HFOC_SSH_SESSION_PROTOCOL_ERROR,
          "an over-long padding length was accepted");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "packet-padding") == 0,
          "padding reason=%s", hfoc_ssh_session_reason(&session));

    reset_session();
    feed_identification();
    CHECK(feed(empty_payload, sizeof(empty_payload)) ==
              HFOC_SSH_SESSION_PROTOCOL_ERROR,
          "an empty packet payload was accepted");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "packet-payload") == 0,
          "empty-payload reason=%s", hfoc_ssh_session_reason(&session));
}

static void test_preauth_ignore_flood(void)
{
    uint8_t packets[HFOC_SSH_SESSION_PREAUTH_LIMIT + 1U][10];
    enum hfoc_ssh_session_result result;

    for (size_t index = 0U;
         index < sizeof(packets) / sizeof(packets[0]); ++index) {
        packets[index][0] = 0U;
        packets[index][1] = 0U;
        packets[index][2] = 0U;
        packets[index][3] = 6U; /* padding 4 + payload 1 + padding 4 */
        packets[index][4] = 4U;
        packets[index][5] = 2U; /* SSH_MSG_IGNORE */
        packets[index][6] = 0U;
        packets[index][7] = 0U;
        packets[index][8] = 0U;
        packets[index][9] = 0U;
    }
    reset_session();
    feed_identification();
    result = feed(&packets[0][0], sizeof(packets));
    CHECK(result == HFOC_SSH_SESSION_PROTOCOL_ERROR,
          "a coalesced pre-auth IGNORE flood returned %d", (int)result);
    CHECK(hfoc_ssh_session_done(&session),
          "the session survived past the pre-auth message bound");
    CHECK(strcmp(hfoc_ssh_session_reason(&session),
                 "too-many-pre-auth-messages") == 0,
          "pre-auth bound reason=%s", hfoc_ssh_session_reason(&session));
}

static void test_random_input(void)
{
    uint32_t state = 0x12345678U;
    uint8_t bytes[512];

    for (uint32_t round = 0U; round < 16U; ++round) {
        enum hfoc_ssh_session_result result;

        reset_session();
        feed_identification();
        for (size_t index = 0U; index < sizeof(bytes); ++index) {
            state = state * 1664525U + 1013904223U;
            bytes[index] = (uint8_t)(state >> 24);
        }
        result = feed(bytes, sizeof(bytes));
        CHECK(result == HFOC_SSH_SESSION_NEED_MORE ||
                  result == HFOC_SSH_SESSION_PROTOCOL_ERROR ||
                  result == HFOC_SSH_SESSION_CRYPTO_ERROR ||
                  result == HFOC_SSH_SESSION_CLOSED,
              "random input returned unknown result %d", (int)result);
        if (result == HFOC_SSH_SESSION_PROTOCOL_ERROR ||
            result == HFOC_SSH_SESSION_CRYPTO_ERROR) {
            CHECK(strcmp(hfoc_ssh_session_reason(&session), "none") != 0,
                  "random input failed without a named reason");
        }
        CHECK(session.input_length <= HFOC_SSH_SESSION_INPUT_MAX,
              "random input overran the input bound");
    }
}

/*
 * ITEM-616: a stock OpenSSH client sends a much larger KEXINIT than the old
 * 2048-byte exchange-hash buffer allowed.  These fixtures drive the real
 * transport with a standard-size MAC name list, then with an over-bound and an
 * unsupported one, so the proof fails on the exact named reason instead of a
 * copied algorithm.
 */
static size_t fill_name_list(char *out, size_t capacity)
{
    static const char token[] = "hmac-sha2-256-etm@openssh.com,";
    size_t at = 0U;

    while (at + sizeof(token) - 1U <= capacity) {
        memcpy(out + at, token, sizeof(token) - 1U);
        at += sizeof(token) - 1U;
    }
    if (at != 0U && out[at - 1U] == ',') {
        --at;
    }
    return at;
}

static size_t build_kexinit_payload(uint8_t *payload, size_t capacity,
                                    const char *kex, const char *macs,
                                    size_t macs_length)
{
    struct hfoc_ssh_writer writer;
    static const uint8_t cookie[16] = {0U};
    static const char host[] = "ecdsa-sha2-nistp256";
    static const char ciphers[] =
        "aes128-gcm@openssh.com,aes256-gcm@openssh.com";
    static const char none[] = "none";
    static const char empty[] = "";

    hfoc_ssh_writer_init(&writer, payload, capacity);
    (void)hfoc_ssh_write_u8(&writer, 20U); /* SSH_MSG_KEXINIT */
    (void)hfoc_ssh_write_bytes(&writer, cookie, sizeof(cookie));
    (void)hfoc_ssh_write_cstring(&writer, kex);
    (void)hfoc_ssh_write_cstring(&writer, host);
    (void)hfoc_ssh_write_cstring(&writer, ciphers);
    (void)hfoc_ssh_write_cstring(&writer, ciphers);
    (void)hfoc_ssh_write_string(&writer, macs, macs_length);
    (void)hfoc_ssh_write_string(&writer, macs, macs_length);
    (void)hfoc_ssh_write_cstring(&writer, none);
    (void)hfoc_ssh_write_cstring(&writer, none);
    (void)hfoc_ssh_write_cstring(&writer, empty);
    (void)hfoc_ssh_write_cstring(&writer, empty);
    (void)hfoc_ssh_write_bool(&writer, false);
    (void)hfoc_ssh_write_u32(&writer, 0U);
    CHECK(!writer.failed, "the KEXINIT fixture exceeded its buffer");
    return writer.failed ? 0U : writer.length;
}

static size_t frame_packet(uint8_t *packet, size_t capacity,
                           const uint8_t *payload, size_t payload_length)
{
    size_t padding = 8U - ((4U + 1U + payload_length) % 8U);
    size_t packet_length;

    if (padding < 4U) {
        padding += 8U;
    }
    packet_length = 1U + payload_length + padding;
    if (4U + packet_length > capacity) {
        CHECK(0, "the packet fixture exceeded its buffer");
        return 0U;
    }
    packet[0] = (uint8_t)(packet_length >> 24);
    packet[1] = (uint8_t)(packet_length >> 16);
    packet[2] = (uint8_t)(packet_length >> 8);
    packet[3] = (uint8_t)packet_length;
    packet[4] = (uint8_t)padding;
    memcpy(packet + 5U, payload, payload_length);
    memset(packet + 5U + payload_length, 0U, padding);
    return 4U + packet_length;
}

static size_t build_ecdh_init(uint8_t *payload, size_t capacity)
{
    /* The NIST P-256 generator is a valid public point; the shared secret is
     * still computed from the server's fresh ephemeral scalar. */
    static const uint8_t generator[65] = {
        0x04U,
        0x6bU, 0x17U, 0xd1U, 0xf2U, 0xe1U, 0x2cU, 0x42U, 0x47U,
        0xf8U, 0xbcU, 0xe6U, 0xe5U, 0x63U, 0xa4U, 0x40U, 0xf2U,
        0x77U, 0x03U, 0x7dU, 0x81U, 0x2dU, 0xebU, 0x33U, 0xa0U,
        0xf4U, 0xa1U, 0x39U, 0x45U, 0xd8U, 0x98U, 0xc2U, 0x96U,
        0x4fU, 0xe3U, 0x42U, 0xe2U, 0xfeU, 0x1aU, 0x7fU, 0x9bU,
        0x8eU, 0xe7U, 0xebU, 0x4aU, 0x7cU, 0x0fU, 0x9eU, 0x16U,
        0x2bU, 0xceU, 0x33U, 0x57U, 0x6bU, 0x31U, 0x5eU, 0xceU,
        0xcbU, 0xb6U, 0x40U, 0x68U, 0x37U, 0xbfU, 0x51U, 0xf5U,
    };
    struct hfoc_ssh_writer writer;

    hfoc_ssh_writer_init(&writer, payload, capacity);
    (void)hfoc_ssh_write_u8(&writer, 30U); /* SSH_MSG_KEX_ECDH_INIT */
    (void)hfoc_ssh_write_string(&writer, generator, sizeof(generator));
    CHECK(!writer.failed, "the KEX_ECDH_INIT fixture exceeded its buffer");
    return writer.failed ? 0U : writer.length;
}

static void prepare_kex_session(void)
{
    reset_session();
    CHECK(hfoc_ssh_crypto_host_key_generate(&session.crypto, host_rng, NULL),
          "the KEXINIT proof could not create a host key");
    feed_identification();
    CHECK(session.state == HFOC_SSH_SESSION_STATE_KEXINIT,
          "the session did not wait for the client KEXINIT (state=%d)",
          (int)session.state);
}

static bool output_has_message(const uint8_t *data, size_t length,
                               uint8_t message)
{
    size_t at = 0U;

    /* reset_session() drops the identification output; tolerate both a
     * leading version line and a stream that starts with the first packet. */
    if (length >= 4U && memcmp(data, "SSH-", 4U) == 0) {
        while (at + 1U < length &&
               !(data[at] == '\r' && data[at + 1U] == '\n')) {
            ++at;
        }
        if (at + 1U < length) {
            at += 2U;
        }
    }
    while (at + 5U <= length) {
        const uint32_t packet_length =
            ((uint32_t)data[at] << 24) | ((uint32_t)data[at + 1U] << 16) |
            ((uint32_t)data[at + 2U] << 8) | (uint32_t)data[at + 3U];

        if (packet_length < 2U || at + 4U + packet_length > length) {
            return false;
        }
        if (data[at + 5U] == message) {
            return true;
        }
        at += 4U + packet_length;
    }
    return false;
}

static void test_large_kexinit(void)
{
    static const char kex[] =
        "ecdh-sha2-nistp256,kex-strict-c-v00@openssh.com";
    uint8_t kexinit[HFOC_SSH_SESSION_KEXINIT_MAX];
    uint8_t packet[HFOC_SSH_SESSION_KEXINIT_MAX + 32U];
    uint8_t ecdh_payload[1U + 4U + 65U];
    uint8_t ecdh_packet[sizeof(ecdh_payload) + 32U];
    uint8_t coalesced[sizeof(packet) + sizeof(ecdh_packet)];
    char macs[1400];
    const size_t macs_length = fill_name_list(macs, sizeof(macs));
    size_t kexinit_length;
    size_t packet_length;
    size_t ecdh_payload_length;
    size_t ecdh_length;

    CHECK(macs_length > 1200U, "the MAC fixture is too short (%zu)",
          macs_length);
    ecdh_payload_length = build_ecdh_init(ecdh_payload,
                                          sizeof(ecdh_payload));
    ecdh_length = frame_packet(ecdh_packet, sizeof(ecdh_packet),
                               ecdh_payload, ecdh_payload_length);

    /* Standard OpenSSH MAC list: 2048 < transcript <= 4096. */
    prepare_kex_session();
    kexinit_length = build_kexinit_payload(kexinit, sizeof(kexinit), kex, macs,
                                           macs_length);
    CHECK(kexinit_length > 2048U,
          "the standard KEXINIT fixture is only %zu bytes", kexinit_length);
    packet_length = frame_packet(packet, sizeof(packet), kexinit,
                                 kexinit_length);
    CHECK(feed(packet, packet_length) == HFOC_SSH_SESSION_NEED_MORE,
          "the standard KEXINIT returned a terminal result");
    CHECK(session.state == HFOC_SSH_SESSION_STATE_KEX_ECDH,
          "the standard KEXINIT stopped at state=%d reason=%s",
          (int)session.state, hfoc_ssh_session_reason(&session));
    CHECK(feed(ecdh_packet, ecdh_length) == HFOC_SSH_SESSION_NEED_MORE,
          "the standard KEX_ECDH_INIT returned a terminal result");
    CHECK(session.state == HFOC_SSH_SESSION_STATE_NEWKEYS,
          "the standard KEXINIT exchange stopped at state=%d reason=%s",
          (int)session.state, hfoc_ssh_session_reason(&session));
    CHECK(!hfoc_ssh_session_done(&session),
          "the standard KEXINIT exchange failed: %s",
          hfoc_ssh_session_reason(&session));
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "none") == 0,
          "the standard KEXINIT exchange reported reason=%s",
          hfoc_ssh_session_reason(&session));
    CHECK(output_has_message(session.output, session.output_length, 31U),
          "the standard KEXINIT exchange queued no KEX_ECDH_REPLY");
    CHECK(output_has_message(session.output, session.output_length, 21U),
          "the standard KEXINIT exchange queued no NEWKEYS");

    /* The same packet delivered one byte at a time. */
    prepare_kex_session();
    packet_length = frame_packet(packet, sizeof(packet), kexinit,
                                 kexinit_length);
    for (size_t index = 0U; index < packet_length; ++index) {
        CHECK(feed(packet + index, 1U) == HFOC_SSH_SESSION_NEED_MORE,
              "the fragmented KEXINIT stopped at byte %zu", index);
    }
    CHECK(session.state == HFOC_SSH_SESSION_STATE_KEX_ECDH,
          "the fragmented KEXINIT stopped at state=%d reason=%s",
          (int)session.state, hfoc_ssh_session_reason(&session));
    CHECK(feed(ecdh_packet, ecdh_length) == HFOC_SSH_SESSION_NEED_MORE,
          "the fragmented KEXINIT exchange returned a terminal result");
    CHECK(session.state == HFOC_SSH_SESSION_STATE_NEWKEYS,
          "the fragmented KEXINIT exchange stopped at state=%d reason=%s",
          (int)session.state, hfoc_ssh_session_reason(&session));

    /* KEXINIT and KEX_ECDH_INIT coalesced in one read. */
    prepare_kex_session();
    packet_length = frame_packet(packet, sizeof(packet), kexinit,
                                 kexinit_length);
    memcpy(coalesced, packet, packet_length);
    memcpy(coalesced + packet_length, ecdh_packet, ecdh_length);
    CHECK(feed(coalesced, packet_length + ecdh_length) ==
              HFOC_SSH_SESSION_NEED_MORE,
          "the coalesced KEXINIT exchange returned a terminal result");
    CHECK(session.state == HFOC_SSH_SESSION_STATE_NEWKEYS,
          "the coalesced KEXINIT exchange stopped at state=%d reason=%s",
          (int)session.state, hfoc_ssh_session_reason(&session));
}

static void test_kexinit_bounds(void)
{
    static const char kex[] =
        "ecdh-sha2-nistp256,kex-strict-c-v00@openssh.com";
    uint8_t kexinit[HFOC_SSH_SESSION_KEXINIT_MAX + 1024U];
    uint8_t packet[sizeof(kexinit) + 32U];
    char macs[2100];
    const size_t macs_length = fill_name_list(macs, sizeof(macs));
    size_t kexinit_length;
    size_t packet_length;

    prepare_kex_session();
    kexinit_length = build_kexinit_payload(kexinit, sizeof(kexinit), kex, macs,
                                           macs_length);
    CHECK(kexinit_length > HFOC_SSH_SESSION_KEXINIT_MAX,
          "the over-bound KEXINIT fixture is only %zu bytes", kexinit_length);
    packet_length = frame_packet(packet, sizeof(packet), kexinit,
                                 kexinit_length);
    CHECK(feed(packet, packet_length) == HFOC_SSH_SESSION_PROTOCOL_ERROR,
          "an over-bound KEXINIT was accepted");
    CHECK(hfoc_ssh_session_done(&session),
          "the over-bound KEXINIT did not close the session");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "kexinit-too-large") == 0,
          "over-bound KEXINIT reason=%s", hfoc_ssh_session_reason(&session));

    prepare_kex_session();
    kexinit_length = build_kexinit_payload(
        kexinit, sizeof(kexinit), "diffie-hellman-group14-sha256", macs, 100U);
    packet_length = frame_packet(packet, sizeof(packet), kexinit,
                                 kexinit_length);
    CHECK(feed(packet, packet_length) == HFOC_SSH_SESSION_PROTOCOL_ERROR,
          "an unsupported KEX algorithm was accepted");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "no-common-kex") == 0,
          "unsupported KEX reason=%s", hfoc_ssh_session_reason(&session));
}

static void feed_channel_open(uint32_t max_packet)
{
    uint8_t packet[33];
    size_t at = 0U;

    /* The handshake itself is covered by the interop proof; enter the channel
       state directly so this checks the CHANNEL_OPEN validation only. */
    session.state = HFOC_SSH_SESSION_STATE_CHANNEL;
    session.encrypted_rx = false;
    session.channel_open = false;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 29U; /* packet_length */
    packet[at++] = 4U;  /* padding_length */
    packet[at++] = 90U; /* SSH_MSG_CHANNEL_OPEN */
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 7U;  /* "session" */
    memcpy(packet + at, "session", 7U);
    at += 7U;
    packet[at++] = 0U; /* sender channel 1 */
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 1U;
    packet[at++] = 0U; /* window 65536 */
    packet[at++] = 1U;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = (uint8_t)(max_packet >> 24);
    packet[at++] = (uint8_t)(max_packet >> 16);
    packet[at++] = (uint8_t)(max_packet >> 8);
    packet[at++] = (uint8_t)max_packet;
    packet[at++] = 0U; /* padding */
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 0U;
    CHECK(at == sizeof(packet), "the CHANNEL_OPEN fixture built %zu bytes", at);
    (void)feed(packet, sizeof(packet));
}

static void test_channel_open_bounds(void)
{
    reset_session();
    feed_identification();
    feed_channel_open(0U);
    CHECK(hfoc_ssh_session_done(&session),
          "a zero maximum packet size was accepted");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "channel-max-packet") == 0,
          "zero-max-packet reason=%s", hfoc_ssh_session_reason(&session));

    reset_session();
    feed_identification();
    feed_channel_open(65536U);
    CHECK(!hfoc_ssh_session_done(&session),
          "a large maximum packet size closed the channel");
    CHECK(session.channel_open, "the channel did not open");
    CHECK(session.client_max_packet == HFOC_SSH_SESSION_PACKET_MAX,
          "the large maximum packet size was not clamped: %u",
          session.client_max_packet);
}

static void test_auth_failure_bound(void)
{
    uint8_t packet[49];
    size_t at = 0U;

    reset_session();
    session.state = HFOC_SSH_SESSION_STATE_AUTH;
    session.encrypted_rx = false;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 45U; /* packet_length */
    packet[at++] = 4U;  /* padding_length */
    packet[at++] = 50U; /* SSH_MSG_USERAUTH_REQUEST */
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 4U;
    memcpy(packet + at, "hfoc", 4U);
    at += 4U;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 14U;
    memcpy(packet + at, "ssh-connection", 14U);
    at += 14U;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 0U;
    packet[at++] = 8U;
    memcpy(packet + at, "password", 8U);
    at += 8U;
    packet[at++] = 0U; /* change password = false */
    memset(packet + at, 0U, 4U);
    at += 4U;
    CHECK(at == sizeof(packet), "the auth fixture built %zu bytes", at);
    for (uint32_t attempt = 0U; attempt < HFOC_SSH_SESSION_AUTH_LIMIT;
         ++attempt) {
        const enum hfoc_ssh_session_result result =
            feed(packet, sizeof(packet));

        if (attempt + 1U < HFOC_SSH_SESSION_AUTH_LIMIT) {
            CHECK(result == HFOC_SSH_SESSION_NEED_MORE,
                  "auth attempt %u returned %d", attempt + 1U, (int)result);
            CHECK(!hfoc_ssh_session_done(&session),
                  "the session closed before the auth bound");
        } else {
            CHECK(result == HFOC_SSH_SESSION_PROTOCOL_ERROR,
                  "the auth-failure bound returned %d", (int)result);
            CHECK(hfoc_ssh_session_done(&session),
                  "the session stayed open past the auth-failure bound");
            CHECK(strcmp(hfoc_ssh_session_reason(&session),
                         "too-many-auth-failures") == 0,
                  "auth-limit reason=%s",
                  hfoc_ssh_session_reason(&session));
        }
    }
}

static void test_tick_transitions(void)
{
    reset_session();
    hfoc_ssh_session_start(&session, 0U);
    session.state = HFOC_SSH_SESSION_STATE_CHANNEL;
    CHECK(!hfoc_ssh_session_tick(&session, 120000U),
          "the idle deadline did not stop the session");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "idle-timeout") == 0,
          "idle reason=%s", hfoc_ssh_session_reason(&session));

    reset_session();
    hfoc_ssh_session_start(&session, 0U);
    session.state = HFOC_SSH_SESSION_STATE_CHANNEL;
    CHECK(!hfoc_ssh_session_tick(&session, 900000U),
          "the rekey deadline did not close the session");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "rekey-limit") == 0,
          "rekey reason=%s", hfoc_ssh_session_reason(&session));

    reset_session();
    hfoc_ssh_session_start(&session, 0U);
    session.state = HFOC_SSH_SESSION_STATE_CHANNEL;
    CHECK(!hfoc_ssh_session_tick(&session, 1800000U),
          "the absolute deadline did not stop the session");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "absolute-timeout") == 0,
          "absolute reason=%s", hfoc_ssh_session_reason(&session));

    reset_session();
    hfoc_ssh_session_start(&session, 0U);
    session.state = HFOC_SSH_SESSION_STATE_KEX_ECDH;
    CHECK(!hfoc_ssh_session_tick(&session, 20000U),
          "the handshake deadline did not stop the session");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "handshake-timeout") == 0,
          "handshake reason=%s", hfoc_ssh_session_reason(&session));

    reset_session();
    hfoc_ssh_session_start(&session, 0U);
    session.state = HFOC_SSH_SESSION_STATE_AUTH;
    CHECK(!hfoc_ssh_session_tick(&session, 30000U),
          "the auth deadline did not stop the session");
    CHECK(strcmp(hfoc_ssh_session_reason(&session), "auth-timeout") == 0,
          "auth reason=%s", hfoc_ssh_session_reason(&session));
}

int main(void)
{
    urandom_fd = open("/dev/urandom", O_RDONLY);
    CHECK(urandom_fd >= 0, "cannot open /dev/urandom");
    if (urandom_fd < 0) {
        return 1;
    }
    test_fragmented_identification();
    test_identification_bounds();
    test_malformed_packets();
    test_preauth_ignore_flood();
    test_channel_open_bounds();
    test_auth_failure_bound();
    test_random_input();
    test_tick_transitions();
    test_large_kexinit();
    test_kexinit_bounds();
    hfoc_ssh_session_free(&session);
    close(urandom_fd);
    if (failures != 0) {
        fprintf(stderr, "ssh adversarial host: %d failure(s)\n", failures);
        return 1;
    }
    printf("ssh adversarial host: ALL OK\n");
    return 0;
}
