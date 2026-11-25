#ifndef HFOC_SSH_TRANSPORT_H
#define HFOC_SSH_TRANSPORT_H

#include "hfoc_ssh_crypto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HFOC_SSH_SESSION_PACKET_MAX 32768U
#define HFOC_SSH_SESSION_INPUT_MAX 32768U
#define HFOC_SSH_SESSION_OUTPUT_MAX 32768U
/*
 * ITEM-602: one command's full diagnostic output.  The diag report is 96
 * records of at most 192 bytes plus the BEGIN/END frame, and the journal
 * reading is 32 records of at most 440 bytes; 24 KiB carries either without
 * turning the channel into an unbounded buffer.
 */
#define HFOC_SSH_SESSION_EXEC_OUTPUT_MAX 24576U
#define HFOC_SSH_SESSION_VERSION_MAX 256U
#define HFOC_SSH_SESSION_KEXINIT_MAX 4096U
#define HFOC_SSH_SESSION_AUTH_KEY_MAX 192U
#define HFOC_SSH_SESSION_COMMAND_MAX 256U
#define HFOC_SSH_SESSION_WINDOW 65536U
/* ITEM-629: the receive window matches the bounded TTY input queue. */
#define HFOC_SSH_SESSION_SERVER_WINDOW 4096U
#define HFOC_SSH_SESSION_PTY_TERM_MAX 64U
/* RFC 4254 §8: space for all 159 opcode/uint32 pairs and TTY_OP_END. */
#define HFOC_SSH_SESSION_PTY_MODES_MAX (159U * 5U + 1U)
#define HFOC_SSH_SESSION_MAX_PACKET 16384U
#define HFOC_SSH_SESSION_AUTH_LIMIT 3U
#define HFOC_SSH_SESSION_PREAUTH_LIMIT 256U
/* ITEM-601 lifecycle bounds from the approved SSH brief. */
#define HFOC_SSH_SESSION_HANDSHAKE_TIMEOUT_MS 20000U
#define HFOC_SSH_SESSION_AUTH_TIMEOUT_MS 30000U
#define HFOC_SSH_SESSION_IDLE_TIMEOUT_MS 120000U
#define HFOC_SSH_SESSION_ABSOLUTE_TIMEOUT_MS 1800000U
#define HFOC_SSH_SESSION_REKEY_TIME_MS 900000U
#define HFOC_SSH_SESSION_REKEY_BYTES UINT64_C(536870912)

enum hfoc_ssh_session_state {
    HFOC_SSH_SESSION_STATE_IDENT = 0,
    HFOC_SSH_SESSION_STATE_KEXINIT,
    HFOC_SSH_SESSION_STATE_KEX_ECDH,
    HFOC_SSH_SESSION_STATE_NEWKEYS,
    HFOC_SSH_SESSION_STATE_SERVICE,
    HFOC_SSH_SESSION_STATE_AUTH,
    HFOC_SSH_SESSION_STATE_CHANNEL,
    HFOC_SSH_SESSION_STATE_CLOSING,
    HFOC_SSH_SESSION_STATE_DONE,
    HFOC_SSH_SESSION_STATE_FAILED,
};

enum hfoc_ssh_session_result {
    HFOC_SSH_SESSION_OK = 0,
    HFOC_SSH_SESSION_NEED_MORE,
    HFOC_SSH_SESSION_CLOSED,
    HFOC_SSH_SESSION_PROTOCOL_ERROR,
    HFOC_SSH_SESSION_CRYPTO_ERROR,
    HFOC_SSH_SESSION_AUTH_ERROR,
    HFOC_SSH_SESSION_REFUSED,
};

struct hfoc_ssh_session;

/*
 * ITEM-629: the transport parses RFC 4254 pty-req/shell/window-change/signal
 * and delegates the actual terminal and child process to this bounded
 * session owner.  Every callback is called from the network owner's context;
 * output must be non-blocking.
 */
struct hfoc_ssh_shell_ops {
    bool (*start)(void *context, const char *term, uint32_t columns,
                  uint32_t rows);
    bool (*input)(void *context, const uint8_t *data, size_t length,
                  size_t *accepted_out);
    uint32_t (*input_space)(void *context);
    bool (*resize)(void *context, uint32_t columns, uint32_t rows);
    bool (*signal)(void *context, const char *name, size_t name_length);
    size_t (*output)(void *context, uint8_t *buffer, size_t capacity);
    bool (*exit_status)(void *context, int *status_out);
    void (*eof)(void *context);
    void (*hangup)(void *context);
};

typedef int (*hfoc_ssh_command_fn)(void *context, const char *command, uint8_t *out,
                              size_t capacity, size_t *out_length);

struct hfoc_ssh_session {
    struct hfoc_ssh_crypto crypto;
    hfoc_ssh_rng_fn rng;
    void *rng_context;
    hfoc_ssh_command_fn command;
    void *command_context;
    const struct hfoc_ssh_shell_ops *shell_ops;
    void *shell_context;
    uint8_t authorized_key[HFOC_SSH_SESSION_AUTH_KEY_MAX];
    size_t authorized_key_length;
    enum hfoc_ssh_session_state state;
    const char *reason;
    uint8_t input[HFOC_SSH_SESSION_INPUT_MAX];
    size_t input_length;
    uint8_t output[HFOC_SSH_SESSION_OUTPUT_MAX];
    size_t output_length;
    /*
     * ITEM-602: a completed command's output can be larger than one channel
     * window or one output buffer.  It is produced once into this bounded
     * buffer and handed to the channel as the peer's window and the transport's
     * own output buffer make room; the exit-status/EOF/close frame is queued
     * only after the last byte, so a small window delays the close instead of
     * failing the command.
     */
    uint8_t exec_output[HFOC_SSH_SESSION_EXEC_OUTPUT_MAX];
    size_t exec_length;
    size_t exec_offset;
    int exec_status;
    bool exit_sent;
    /* Reused per-packet scratch: no large packet buffer lives on the stack. */
    uint8_t scratch_a[HFOC_SSH_SESSION_PACKET_MAX];
    uint8_t scratch_b[HFOC_SSH_SESSION_PACKET_MAX];
    uint8_t channel_payload[HFOC_SSH_SESSION_MAX_PACKET];
    uint8_t client_version[HFOC_SSH_SESSION_VERSION_MAX];
    size_t client_version_length;
    uint8_t server_version[HFOC_SSH_SESSION_VERSION_MAX];
    size_t server_version_length;
    uint8_t i_c[HFOC_SSH_SESSION_KEXINIT_MAX];
    size_t i_c_length;
    uint8_t i_s[HFOC_SSH_SESSION_KEXINIT_MAX];
    size_t i_s_length;
    uint8_t session_id[32];
    uint8_t exchange_hash[32];
    uint32_t sequence_tx;
    uint32_t sequence_rx;
    uint8_t last_message;
    uint32_t auth_failures;
    uint32_t preauth_packets;
    uint32_t server_channel;
    /* The negotiated key length per direction (16 for AES-128-GCM, 32 for
       AES-256-GCM); the client's preference decides each list. */
    uint8_t cipher_c2s_key_length;
    uint8_t cipher_s2c_key_length;
    uint32_t client_channel;
    uint32_t client_window;
    uint32_t client_max_packet;
    bool strict_kex;
    bool encrypted_tx;
    bool encrypted_rx;
    bool channel_open;
    bool exec_started;
    bool close_sent;
    /* ITEM-629: pty/shell state and the server's receive window. */
    bool pty_requested;
    bool shell_started;
    bool shell_eof;
    uint32_t pty_columns;
    uint32_t pty_rows;
    uint32_t server_window;
    char pty_term[HFOC_SSH_SESSION_PTY_TERM_MAX + 1U];
    uint64_t bytes_tx;
    uint64_t bytes_rx;
    uint32_t started_ms;
    uint32_t last_activity_ms;
    bool started;
};

bool hfoc_ssh_session_init(struct hfoc_ssh_session *session, hfoc_ssh_rng_fn rng,
                      void *rng_context, hfoc_ssh_command_fn command,
                      void *command_context);
void hfoc_ssh_session_set_shell(struct hfoc_ssh_session *session,
                                const struct hfoc_ssh_shell_ops *ops,
                                void *context);
void hfoc_ssh_session_free(struct hfoc_ssh_session *session);
void hfoc_ssh_session_start(struct hfoc_ssh_session *session, uint32_t now_ms);
/*
 * One lifecycle check.  Returns false once the session has been failed or
 * asked to close, so the adapter stops writing and releases it.  The rekey
 * limit is a clean close in v1: a continued session past the limit is not
 * allowed, and a silent one is worse.
 */
bool hfoc_ssh_session_tick(struct hfoc_ssh_session *session, uint32_t now_ms);
enum hfoc_ssh_session_result hfoc_ssh_session_feed(
    struct hfoc_ssh_session *session, const uint8_t *data, size_t length,
    uint32_t now_ms);
size_t hfoc_ssh_session_take_output(struct hfoc_ssh_session *session, uint8_t *data,
                               size_t capacity);
/*
 * ITEM-602: try to hand more of a completed command's output to the channel.
 * The adapter calls this after draining the transport output buffer and after
 * a WINDOW_ADJUST; it is a no-op when the channel is already caught up.
 * Returns false only on a protocol/crypto failure, never merely because the
 * peer's window is momentarily full.
 */
bool hfoc_ssh_session_flush_output(struct hfoc_ssh_session *session);
bool hfoc_ssh_session_done(const struct hfoc_ssh_session *session);
const char *hfoc_ssh_session_reason(const struct hfoc_ssh_session *session);

#endif
