#ifdef HFOC_PRODUCTION_TLS

#include "hfoc_ssh_transport.h"

#include "hfoc_ssh_wire.h"

#include <string.h>

#include "mbedtls/sha256.h"

#define SSH_MSG_DISCONNECT 1U
#define SSH_MSG_IGNORE 2U
#define SSH_MSG_UNIMPLEMENTED 3U
#define SSH_MSG_DEBUG 4U
#define SSH_MSG_SERVICE_REQUEST 5U
#define SSH_MSG_SERVICE_ACCEPT 6U
#define SSH_MSG_KEXINIT 20U
#define SSH_MSG_NEWKEYS 21U
#define SSH_MSG_KEX_ECDH_INIT 30U
#define SSH_MSG_KEX_ECDH_REPLY 31U
#define SSH_MSG_USERAUTH_REQUEST 50U
#define SSH_MSG_USERAUTH_FAILURE 51U
#define SSH_MSG_USERAUTH_SUCCESS 52U
#define SSH_MSG_USERAUTH_PK_OK 60U
#define SSH_MSG_GLOBAL_REQUEST 80U
#define SSH_MSG_REQUEST_FAILURE 82U
#define SSH_MSG_CHANNEL_OPEN 90U
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91U
#define SSH_MSG_CHANNEL_OPEN_FAILURE 92U
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93U
#define SSH_MSG_CHANNEL_DATA 94U
#define SSH_MSG_CHANNEL_EOF 96U
#define SSH_MSG_CHANNEL_CLOSE 97U
#define SSH_MSG_CHANNEL_REQUEST 98U
#define SSH_MSG_CHANNEL_SUCCESS 99U
#define SSH_MSG_CHANNEL_FAILURE 100U

static const char ssh_host_algorithm[] = "ecdsa-sha2-nistp256";

/*
 * ITEM-607: wipe derived keys, IVs and the ECDH shared-secret copy after the
 * AEAD owner has consumed them.  Volatile stores keep the compiler from
 * dropping the wipe as dead state.
 */
static void ssh_secret_zero(void *data, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    while (length != 0U) {
        *bytes++ = 0U;
        --length;
    }
}
static const char ssh_kex_algorithm[] = "ecdh-sha2-nistp256";
static const char ssh_kex_list[] =
    "ecdh-sha2-nistp256,kex-strict-s-v00@openssh.com";
static const char ssh_strict_client[] = "kex-strict-c-v00@openssh.com";
static const char ssh_server_version[] = "SSH-2.0-HFOC_1.0";

static bool out_append(struct hfoc_ssh_session *session, const void *data,
                       size_t length)
{
    if (session->output_length + length > sizeof(session->output)) {
        session->reason = "output-buffer-full";
        return false;
    }
    memcpy(session->output + session->output_length, data, length);
    session->output_length += length;
    return true;
}

static bool queue_packet(struct hfoc_ssh_session *session, const uint8_t *payload,
                         size_t payload_length)
{
    uint8_t *block = session->scratch_a;
    size_t block_size;
    size_t padding;
    uint8_t padding_length;

    if (payload_length == 0U || payload_length > HFOC_SSH_SESSION_PACKET_MAX - 32U) {
        session->reason = "payload-size";
        return false;
    }
    if (!session->encrypted_tx) {
        block_size = 8U;
        padding = block_size - ((4U + 1U + payload_length) % block_size);
        if (padding < 4U) {
            padding += block_size;
        }
        if (1U + payload_length + padding > HFOC_SSH_SESSION_PACKET_MAX) {
            session->reason = "packet-size";
            return false;
        }
        padding_length = (uint8_t)padding;
        block[0] = (uint8_t)((1U + payload_length + padding) >> 24);
        block[1] = (uint8_t)((1U + payload_length + padding) >> 16);
        block[2] = (uint8_t)((1U + payload_length + padding) >> 8);
        block[3] = (uint8_t)(1U + payload_length + padding);
        block[4] = padding_length;
        memcpy(block + 5U, payload, payload_length);
        if (session->rng(session->rng_context, block + 5U + payload_length,
                         padding) != 0) {
            session->reason = "rng";
            return false;
        }
        if (!out_append(session, block, 4U + 1U + payload_length + padding)) {
            return false;
        }
    } else {
        block_size = 16U;
        /* AEAD: the packet_length itself (padding_length+payload+padding)
         * must be a multiple of the cipher block size. */
        padding = block_size - ((1U + payload_length) % block_size);
        if (padding < 4U) {
            padding += block_size;
        }
        if (1U + payload_length + padding > HFOC_SSH_SESSION_PACKET_MAX) {
            session->reason = "packet-size";
            return false;
        }
        padding_length = (uint8_t)padding;
        block[0] = (uint8_t)((1U + payload_length + padding) >> 24);
        block[1] = (uint8_t)((1U + payload_length + padding) >> 16);
        block[2] = (uint8_t)((1U + payload_length + padding) >> 8);
        block[3] = (uint8_t)(1U + payload_length + padding);
        block[4] = padding_length;
        memcpy(block + 5U, payload, payload_length);
        if (session->rng(session->rng_context, block + 5U + payload_length,
                         padding) != 0) {
            session->reason = "rng";
            return false;
        }
        {
            uint8_t *cipher = session->scratch_b;
            uint8_t tag[16];
            size_t encrypted_length = 1U + payload_length + padding;

            if (!hfoc_ssh_crypto_seal(&session->crypto, block, block + 4U,
                                 encrypted_length, cipher, tag)) {
                session->reason = "aead-seal";
                return false;
            }
            /*
             * All three pieces or none: a partial length/ciphertext prefix
             * would leave the output stream unusable and the session would be
             * torn down with a legitimate command still queued.
             */
            if (session->output_length + 4U + encrypted_length +
                    sizeof(tag) >
                sizeof(session->output)) {
                session->reason = "output-buffer-full";
                return false;
            }
            (void)out_append(session, block, 4U);
            (void)out_append(session, cipher, encrypted_length);
            (void)out_append(session, tag, sizeof(tag));
        }
    }
    ++session->sequence_tx;
    session->bytes_tx += 4U + 1U + payload_length + padding;
    return true;
}

static bool queue_message(struct hfoc_ssh_session *session, uint8_t message)
{
    return queue_packet(session, &message, 1U);
}

static bool send_disconnect(struct hfoc_ssh_session *session, uint32_t reason,
                            const char *description)
{
    uint8_t payload[256];
    struct hfoc_ssh_writer writer;

    hfoc_ssh_writer_init(&writer, payload, sizeof(payload));
    (void)hfoc_ssh_write_u8(&writer, SSH_MSG_DISCONNECT);
    (void)hfoc_ssh_write_u32(&writer, reason);
    (void)hfoc_ssh_write_cstring(&writer, description);
    (void)hfoc_ssh_write_cstring(&writer, "");
    if (writer.failed) {
        return false;
    }
    (void)queue_packet(session, payload, writer.length);
    session->state = HFOC_SSH_SESSION_STATE_FAILED;
    session->reason = description;
    return false;
}

static bool build_kexinit(struct hfoc_ssh_session *session)
{
    uint8_t cookie[16];
    struct hfoc_ssh_writer writer;

    if (session->rng(session->rng_context, cookie, sizeof(cookie)) != 0) {
        session->reason = "rng";
        return false;
    }
    hfoc_ssh_writer_init(&writer, session->i_s, sizeof(session->i_s));
    (void)hfoc_ssh_write_u8(&writer, SSH_MSG_KEXINIT);
    (void)hfoc_ssh_write_bytes(&writer, cookie, sizeof(cookie));
    (void)hfoc_ssh_write_cstring(&writer, ssh_kex_list);
    (void)hfoc_ssh_write_cstring(&writer, ssh_host_algorithm);
    (void)hfoc_ssh_write_cstring(&writer,
                            "aes128-gcm@openssh.com,aes256-gcm@openssh.com");
    (void)hfoc_ssh_write_cstring(&writer,
                            "aes128-gcm@openssh.com,aes256-gcm@openssh.com");
    (void)hfoc_ssh_write_cstring(&writer,
                            "hmac-sha2-256-etm@openssh.com,hmac-sha2-256");
    (void)hfoc_ssh_write_cstring(&writer,
                            "hmac-sha2-256-etm@openssh.com,hmac-sha2-256");
    (void)hfoc_ssh_write_cstring(&writer, "none");
    (void)hfoc_ssh_write_cstring(&writer, "none");
    (void)hfoc_ssh_write_cstring(&writer, "");
    (void)hfoc_ssh_write_cstring(&writer, "");
    (void)hfoc_ssh_write_bool(&writer, false);
    (void)hfoc_ssh_write_u32(&writer, 0U);
    if (writer.failed) {
        session->reason = "kexinit-size";
        return false;
    }
    session->i_s_length = writer.length;
    if (!queue_packet(session, session->i_s, session->i_s_length)) {
        return false;
    }
    session->state = HFOC_SSH_SESSION_STATE_KEXINIT;
    return true;
}

static bool read_raw(struct hfoc_ssh_reader *reader, const uint8_t **data,
                     size_t length)
{
    if (reader->failed || length > reader->length - reader->offset) {
        reader->failed = true;
        return false;
    }
    *data = reader->data + reader->offset;
    reader->offset += length;
    return true;
}

/*
 * The client's name-list order decides the cipher (RFC 4253 section 7.1);
 * the server supports both GCM sizes, so the first supported name is the
 * negotiated one.  Returns the key length in bytes, or zero when there is no
 * common cipher.
 */
static uint8_t ssh_cipher_key_length(const uint8_t *list, uint32_t length)
{
    static const char aes128[] = "aes128-gcm@openssh.com";
    static const char aes256[] = "aes256-gcm@openssh.com";
    uint32_t start = 0U;

    if (list == NULL) {
        return 0U;
    }
    for (uint32_t index = 0U; index <= length; ++index) {
        if (index == length || list[index] == ',') {
            const size_t name_length = index - start;

            if (name_length == sizeof(aes128) - 1U &&
                memcmp(list + start, aes128, name_length) == 0) {
                return 16U;
            }
            if (name_length == sizeof(aes256) - 1U &&
                memcmp(list + start, aes256, name_length) == 0) {
                return 32U;
            }
            start = index + 1U;
        }
    }
    return 0U;
}

static bool parse_kexinit(struct hfoc_ssh_session *session,
                          const uint8_t *payload, size_t payload_length)
{
    struct hfoc_ssh_reader reader;
    const uint8_t *cookie;
    const uint8_t *lists[10];
    uint32_t lengths[10];
    bool first_kex_follows;
    uint32_t reserved;
    size_t index;

    if (session->state != HFOC_SSH_SESSION_STATE_IDENT &&
        session->state != HFOC_SSH_SESSION_STATE_KEXINIT) {
        return send_disconnect(session, 2U, "unexpected-kexinit");
    }
    if (payload_length > sizeof(session->i_c)) {
        return send_disconnect(session, 2U, "kexinit-too-large");
    }
    memcpy(session->i_c, payload, payload_length);
    session->i_c_length = payload_length;
    hfoc_ssh_reader_init(&reader, payload, payload_length);
    {
        uint8_t message;
        if (!hfoc_ssh_read_u8(&reader, &message) || message != SSH_MSG_KEXINIT ||
            !read_raw(&reader, &cookie, 16U)) {
            return send_disconnect(session, 2U, "kexinit-parse");
        }
    }
    for (index = 0U; index < 10U; ++index) {
        if (!hfoc_ssh_read_string(&reader, &lists[index], &lengths[index])) {
            return send_disconnect(session, 2U, "kexinit-lists");
        }
    }
    if (!hfoc_ssh_read_bool(&reader, &first_kex_follows) ||
        !hfoc_ssh_read_u32(&reader, &reserved) || !hfoc_ssh_reader_done(&reader)) {
        return send_disconnect(session, 2U, "kexinit-tail");
    }
    if (!hfoc_ssh_name_list_has(lists[0], lengths[0], ssh_kex_algorithm)) {
        return send_disconnect(session, 3U, "no-common-kex");
    }
    if (!hfoc_ssh_name_list_has(lists[1], lengths[1], ssh_host_algorithm)) {
        return send_disconnect(session, 3U, "no-common-host-key");
    }
    session->cipher_c2s_key_length =
        ssh_cipher_key_length(lists[2], lengths[2]);
    session->cipher_s2c_key_length =
        ssh_cipher_key_length(lists[3], lengths[3]);
    if (session->cipher_c2s_key_length == 0U) {
        return send_disconnect(session, 3U, "no-common-cipher-c2s");
    }
    if (session->cipher_s2c_key_length == 0U) {
        return send_disconnect(session, 3U, "no-common-cipher-s2c");
    }
    if (!hfoc_ssh_name_list_has(lists[6], lengths[6], "none") ||
        !hfoc_ssh_name_list_has(lists[7], lengths[7], "none")) {
        return send_disconnect(session, 3U, "no-compression-none");
    }
    session->strict_kex =
        hfoc_ssh_name_list_has(lists[0], lengths[0], ssh_strict_client);
    if (!hfoc_ssh_crypto_kex_start(&session->crypto, session->rng,
                              session->rng_context)) {
        return send_disconnect(session, 2U, "kex-ephemeral");
    }
    session->state = HFOC_SSH_SESSION_STATE_KEX_ECDH;
    return true;
}

static bool derive_and_init_keys(struct hfoc_ssh_session *session,
                                 bool tx, bool rx)
{
    uint8_t iv[12];
    uint8_t key[32];
    const size_t key_length =
        tx ? session->cipher_s2c_key_length : session->cipher_c2s_key_length;
    bool ok = false;

    if (key_length != 16U && key_length != 32U) {
        session->reason = "cipher-key-length";
        goto done;
    }
    if (!hfoc_ssh_crypto_kdf(&session->crypto, session->exchange_hash,
                        session->session_id, tx ? 'B' : 'A', iv,
                        sizeof(iv))) {
        session->reason = "kdf-iv";
        goto done;
    }
    if (!hfoc_ssh_crypto_kdf(&session->crypto, session->exchange_hash,
                        session->session_id, tx ? 'D' : 'C', key,
                        key_length)) {
        session->reason = "kdf-key";
        goto done;
    }
    if (tx) {
        ok = hfoc_ssh_crypto_aead_init(&session->crypto, true, key, key_length,
                                       iv);
    } else if (rx) {
        ok = hfoc_ssh_crypto_aead_init(&session->crypto, false, key, key_length,
                                       iv);
    } else {
        ok = true;
    }
done:
    ssh_secret_zero(iv, sizeof(iv));
    ssh_secret_zero(key, sizeof(key));
    return ok;
}

static bool handle_kex_ecdh(struct hfoc_ssh_session *session,
                            const uint8_t *payload, size_t payload_length)
{
    struct hfoc_ssh_reader reader;
    const uint8_t *q_c;
    uint32_t q_c_length;
    uint8_t shared[HFOC_SSH_CRYPTO_SHARED_MAX];
    size_t shared_length = 0U;
    uint8_t signature[HFOC_SSH_CRYPTO_SIGNATURE_MAX];
    size_t signature_length = sizeof(signature);
    uint8_t reply[512];
    struct hfoc_ssh_writer writer;

    if (session->state != HFOC_SSH_SESSION_STATE_KEX_ECDH) {
        return send_disconnect(session, 2U, "unexpected-kex-ecdh");
    }
    hfoc_ssh_reader_init(&reader, payload, payload_length);
    {
        uint8_t message;
        if (!hfoc_ssh_read_u8(&reader, &message) ||
            message != SSH_MSG_KEX_ECDH_INIT ||
            !hfoc_ssh_read_string(&reader, &q_c, &q_c_length) ||
            !hfoc_ssh_reader_done(&reader)) {
            return send_disconnect(session, 2U, "kex-ecdh-parse");
        }
    }
    if (q_c_length != 65U) {
        return send_disconnect(session, 2U, "kex-ecdh-point");
    }
    if (!hfoc_ssh_crypto_kex_shared(&session->crypto, q_c, q_c_length,
                               session->rng, session->rng_context, shared,
                               &shared_length)) {
        ssh_secret_zero(shared, sizeof(shared));
        return send_disconnect(session, 2U, "kex-ecdh-shared");
    }
    if (!hfoc_ssh_crypto_exchange_hash(
            &session->crypto, session->client_version,
            session->client_version_length, session->server_version,
            session->server_version_length, session->i_c, session->i_c_length,
            session->i_s, session->i_s_length, q_c, q_c_length,
            session->exchange_hash)) {
        ssh_secret_zero(shared, sizeof(shared));
        return send_disconnect(session, 2U, "exchange-hash");
    }
    memcpy(session->session_id, session->exchange_hash,
           sizeof(session->session_id));
    if (!hfoc_ssh_crypto_sign(&session->crypto, session->exchange_hash,
                         session->rng, session->rng_context, signature,
                         &signature_length)) {
        ssh_secret_zero(shared, sizeof(shared));
        return send_disconnect(session, 2U, "host-key-sign");
    }
    hfoc_ssh_writer_init(&writer, reply, sizeof(reply));
    (void)hfoc_ssh_write_u8(&writer, SSH_MSG_KEX_ECDH_REPLY);
    (void)hfoc_ssh_write_string(&writer, session->crypto.host_blob,
                           session->crypto.host_blob_length);
    (void)hfoc_ssh_write_string(&writer, hfoc_ssh_crypto_kex_public(&session->crypto),
                           65U);
    (void)hfoc_ssh_write_string(&writer, signature, signature_length);
    if (writer.failed) {
        ssh_secret_zero(shared, sizeof(shared));
        return send_disconnect(session, 2U, "kex-reply-size");
    }
    if (!queue_packet(session, reply, writer.length) ||
        !queue_message(session, SSH_MSG_NEWKEYS)) {
        ssh_secret_zero(shared, sizeof(shared));
        return false;
    }
    if (!derive_and_init_keys(session, true, false)) {
        ssh_secret_zero(shared, sizeof(shared));
        return send_disconnect(session, 2U, "kdf-tx");
    }
    session->encrypted_tx = true;
    if (session->strict_kex) {
        session->sequence_tx = 0U;
    }
    session->state = HFOC_SSH_SESSION_STATE_NEWKEYS;
    ssh_secret_zero(shared, sizeof(shared));
    return true;
}

static bool build_signed_auth_data(struct hfoc_ssh_session *session,
                                   const uint8_t *user, uint32_t user_length,
                                   const uint8_t *service,
                                   uint32_t service_length,
                                   const uint8_t *key, uint32_t key_length,
                                   uint8_t *data, size_t capacity,
                                   size_t *data_length)
{
    struct hfoc_ssh_writer writer;

    hfoc_ssh_writer_init(&writer, data, capacity);
    (void)hfoc_ssh_write_string(&writer, session->session_id,
                           sizeof(session->session_id));
    (void)hfoc_ssh_write_u8(&writer, SSH_MSG_USERAUTH_REQUEST);
    (void)hfoc_ssh_write_string(&writer, user, user_length);
    (void)hfoc_ssh_write_string(&writer, service, service_length);
    (void)hfoc_ssh_write_cstring(&writer, "publickey");
    (void)hfoc_ssh_write_bool(&writer, true);
    (void)hfoc_ssh_write_cstring(&writer, ssh_host_algorithm);
    (void)hfoc_ssh_write_string(&writer, key, key_length);
    if (writer.failed) {
        return false;
    }
    *data_length = writer.length;
    return true;
}

static bool send_auth_failure(struct hfoc_ssh_session *session)
{
    uint8_t payload[128];
    struct hfoc_ssh_writer writer;

    ++session->auth_failures;
    hfoc_ssh_writer_init(&writer, payload, sizeof(payload));
    (void)hfoc_ssh_write_u8(&writer, SSH_MSG_USERAUTH_FAILURE);
    (void)hfoc_ssh_write_cstring(&writer, "publickey");
    (void)hfoc_ssh_write_bool(&writer, false);
    if (writer.failed || !queue_packet(session, payload, writer.length)) {
        return false;
    }
    if (session->auth_failures >= HFOC_SSH_SESSION_AUTH_LIMIT) {
        return send_disconnect(session, 2U, "too-many-auth-failures");
    }
    return true;
}

static bool handle_userauth(struct hfoc_ssh_session *session,
                            const uint8_t *payload, size_t payload_length)
{
    struct hfoc_ssh_reader reader;
    const uint8_t *user;
    const uint8_t *service;
    const uint8_t *method;
    const uint8_t *algorithm;
    const uint8_t *key;
    const uint8_t *signature = NULL;
    uint32_t user_length;
    uint32_t service_length;
    uint32_t method_length;
    uint32_t algorithm_length;
    uint32_t key_length;
    uint32_t signature_length = 0U;
    bool has_signature;

    if (session->state != HFOC_SSH_SESSION_STATE_AUTH) {
        return send_disconnect(session, 2U, "unexpected-userauth");
    }
    hfoc_ssh_reader_init(&reader, payload, payload_length);
    {
        uint8_t message;
        if (!hfoc_ssh_read_u8(&reader, &message) ||
            message != SSH_MSG_USERAUTH_REQUEST ||
            !hfoc_ssh_read_string(&reader, &user, &user_length) ||
            !hfoc_ssh_read_string(&reader, &service, &service_length) ||
            !hfoc_ssh_read_string(&reader, &method, &method_length)) {
            return send_disconnect(session, 2U, "userauth-parse");
        }
    }
    if (method_length != 9U || memcmp(method, "publickey", 9U) != 0) {
        return send_auth_failure(session);
    }
    if (!hfoc_ssh_read_bool(&reader, &has_signature) ||
        !hfoc_ssh_read_string(&reader, &algorithm, &algorithm_length) ||
        !hfoc_ssh_read_string(&reader, &key, &key_length)) {
        return send_disconnect(session, 2U, "userauth-publickey-parse");
    }
    if (has_signature &&
        !hfoc_ssh_read_string(&reader, &signature, &signature_length)) {
        return send_disconnect(session, 2U, "userauth-signature");
    }
    if (!hfoc_ssh_reader_done(&reader)) {
        return send_disconnect(session, 2U, "userauth-tail");
    }
    if (user_length != 4U || memcmp(user, "hfoc", 4U) != 0 ||
        service_length != 14U ||
        memcmp(service, "ssh-connection", 14U) != 0 ||
        algorithm_length != strlen(ssh_host_algorithm) ||
        memcmp(algorithm, ssh_host_algorithm, algorithm_length) != 0 ||
        key_length != session->authorized_key_length ||
        memcmp(key, session->authorized_key, key_length) != 0) {
        return send_auth_failure(session);
    }
    if (!has_signature) {
        uint8_t reply[256];
        struct hfoc_ssh_writer writer;

        hfoc_ssh_writer_init(&writer, reply, sizeof(reply));
        (void)hfoc_ssh_write_u8(&writer, SSH_MSG_USERAUTH_PK_OK);
        (void)hfoc_ssh_write_cstring(&writer, ssh_host_algorithm);
        (void)hfoc_ssh_write_string(&writer, key, key_length);
        if (writer.failed || !queue_packet(session, reply, writer.length)) {
            return false;
        }
        return true;
    }
    {
        uint8_t signed_data[512];
        size_t signed_length = 0U;
        uint8_t hash[32];

        if (!build_signed_auth_data(session, user, user_length, service,
                                    service_length, key, key_length,
                                    signed_data, sizeof(signed_data),
                                    &signed_length) ||
            mbedtls_sha256(signed_data, signed_length, hash, 0) != 0) {
            return send_disconnect(session, 2U, "auth-hash");
        }
        if (!hfoc_ssh_crypto_verify_client(key, key_length, hash, signature,
                                      signature_length)) {
            return send_auth_failure(session);
        }
    }
    if (!queue_message(session, SSH_MSG_USERAUTH_SUCCESS)) {
        return false;
    }
    session->state = HFOC_SSH_SESSION_STATE_CHANNEL;
    return true;
}

/*
 * ITEM-602: a bounded-output command is handed to the channel as the peer's
 * window and the transport's own output buffer make room.  Neither limit is a
 * command failure: the adapter drains the buffer, a WINDOW_ADJUST opens the
 * peer side, and the remaining bytes follow.  The exit-status/EOF/close frame
 * is queued only after the last byte.
 */
static bool channel_has_room(const struct hfoc_ssh_session *session,
                             size_t payload_length)
{
    /* Worst case AEAD framing: 4-byte length + 1 pad length + up to 19 pad
       + the 16-byte GCM tag. */
    const size_t packet = 4U + 1U + payload_length + 19U + 16U;

    return session->output_length <= sizeof(session->output) &&
           packet <= sizeof(session->output) - session->output_length;
}

static bool send_exit_frames(struct hfoc_ssh_session *session, int status)
{
    if (!channel_has_room(session, 160U)) {
        return true;
    }
    {
        uint8_t payload[128];
        struct hfoc_ssh_writer writer;

        hfoc_ssh_writer_init(&writer, payload, sizeof(payload));
        (void)hfoc_ssh_write_u8(&writer, SSH_MSG_CHANNEL_REQUEST);
        (void)hfoc_ssh_write_u32(&writer, session->client_channel);
        (void)hfoc_ssh_write_cstring(&writer, "exit-status");
        (void)hfoc_ssh_write_bool(&writer, false);
        (void)hfoc_ssh_write_u32(&writer, (uint32_t)status);
        if (writer.failed || !queue_packet(session, payload, writer.length)) {
            return false;
        }
    }
    {
        uint8_t payload[16];
        struct hfoc_ssh_writer writer;

        hfoc_ssh_writer_init(&writer, payload, sizeof(payload));
        (void)hfoc_ssh_write_u8(&writer, SSH_MSG_CHANNEL_EOF);
        (void)hfoc_ssh_write_u32(&writer, session->client_channel);
        if (writer.failed || !queue_packet(session, payload, writer.length)) {
            return false;
        }
        hfoc_ssh_writer_init(&writer, payload, sizeof(payload));
        (void)hfoc_ssh_write_u8(&writer, SSH_MSG_CHANNEL_CLOSE);
        (void)hfoc_ssh_write_u32(&writer, session->client_channel);
        if (writer.failed || !queue_packet(session, payload, writer.length)) {
            return false;
        }
    }
    session->exit_sent = true;
    session->close_sent = true;
    session->state = HFOC_SSH_SESSION_STATE_CLOSING;
    return true;
}

/*
 * ITEM-629: a shell's bytes are produced by the session owner and drained
 * through the same peer window and output buffer as the four command replies.
 * The TTY is never read into a scratch buffer before there is room to send it,
 * because taking and then dropping bytes would silently lose terminal output.
 */
static bool flush_shell_output(struct hfoc_ssh_session *session)
{
    uint32_t chunks = 0U;
    int status = 0;
    bool drained = false;

    if (session->shell_ops == NULL || session->shell_ops->output == NULL) {
        return true;
    }
    /*
     * Replenish the peer's receive window only as the TTY input queue drains.
     * The initial window equals HFOC_TTY_INPUT_MAX, so an overrun is a
     * protocol error rather than a silently truncated paste.
     */
    if (session->shell_ops->input_space != NULL &&
        session->server_window < HFOC_SSH_SESSION_SERVER_WINDOW) {
        const uint32_t free_space =
            session->shell_ops->input_space(session->shell_context);
        uint32_t increment = HFOC_SSH_SESSION_SERVER_WINDOW -
                             session->server_window;

        if (increment > free_space) {
            increment = free_space;
        }
        if (increment != 0U) {
            uint8_t adjust[16];
            struct hfoc_ssh_writer writer;

            hfoc_ssh_writer_init(&writer, adjust, sizeof(adjust));
            (void)hfoc_ssh_write_u8(&writer, SSH_MSG_CHANNEL_WINDOW_ADJUST);
            (void)hfoc_ssh_write_u32(&writer, session->client_channel);
            (void)hfoc_ssh_write_u32(&writer, increment);
            if (writer.failed ||
                !queue_packet(session, adjust, writer.length)) {
                return false;
            }
            session->server_window += increment;
        }
    }
    for (;;) {
        size_t limit = sizeof(session->exec_output);
        size_t got;

        if (chunks >= 16U || session->client_window == 0U) {
            break;
        }
        if (limit > HFOC_SSH_SESSION_MAX_PACKET) {
            limit = HFOC_SSH_SESSION_MAX_PACKET;
        }
        if (session->client_max_packet != 0U &&
            limit > session->client_max_packet) {
            limit = session->client_max_packet;
        }
        if (limit > session->client_window) {
            limit = session->client_window;
        }
        if (limit == 0U || !channel_has_room(session, 9U + limit)) {
            break;
        }
        got = session->shell_ops->output(session->shell_context,
                                         session->exec_output, limit);
        if (got == 0U) {
            drained = true;
            break;
        }
        {
            uint8_t *payload = session->channel_payload;
            struct hfoc_ssh_writer writer;

            hfoc_ssh_writer_init(&writer, payload,
                                 sizeof(session->channel_payload));
            (void)hfoc_ssh_write_u8(&writer, SSH_MSG_CHANNEL_DATA);
            (void)hfoc_ssh_write_u32(&writer, session->client_channel);
            (void)hfoc_ssh_write_string(&writer, session->exec_output, got);
            if (writer.failed ||
                !queue_packet(session, payload, writer.length)) {
                return false;
            }
        }
        session->client_window -= (uint32_t)got;
        ++chunks;
    }
    if (drained && session->shell_ops->exit_status != NULL &&
        session->shell_ops->exit_status(session->shell_context, &status)) {
        if (!session->exit_sent) {
            return send_exit_frames(session, status);
        }
    }
    return true;
}

static bool flush_exec_output(struct hfoc_ssh_session *session)
{
    uint32_t chunks = 0U;

    if (session->state == HFOC_SSH_SESSION_STATE_DONE ||
        session->state == HFOC_SSH_SESSION_STATE_FAILED) {
        return true;
    }
    if (session->exit_sent) {
        return true;
    }
    if (session->shell_started) {
        return flush_shell_output(session);
    }
    if (!session->exec_started) {
        return true;
    }
    while (session->exec_offset < session->exec_length) {
        size_t chunk = session->exec_length - session->exec_offset;
        uint8_t *payload = session->channel_payload;
        struct hfoc_ssh_writer writer;

        if (chunk > HFOC_SSH_SESSION_MAX_PACKET) {
            chunk = HFOC_SSH_SESSION_MAX_PACKET;
        }
        if (session->client_max_packet != 0U &&
            chunk > session->client_max_packet) {
            chunk = session->client_max_packet;
        }
        if (chunk > session->client_window) {
            chunk = session->client_window;
        }
        if (chunk == 0U) {
            return true; /* the peer has not opened its window yet */
        }
        if (!channel_has_room(session, 9U + chunk)) {
            return true; /* the adapter will drain and call back */
        }
        hfoc_ssh_writer_init(&writer, payload, sizeof(session->channel_payload));
        (void)hfoc_ssh_write_u8(&writer, SSH_MSG_CHANNEL_DATA);
        (void)hfoc_ssh_write_u32(&writer, session->client_channel);
        (void)hfoc_ssh_write_string(
            &writer, session->exec_output + session->exec_offset, chunk);
        if (writer.failed || !queue_packet(session, payload, writer.length)) {
            return false;
        }
        session->client_window -= (uint32_t)chunk;
        session->exec_offset += chunk;
        /* A peer that advertises a tiny maximum packet must not make one
           event-loop turn encode tens of thousands of packets. */
        if (++chunks >= 16U) {
            return true;
        }
    }
    return send_exit_frames(session, session->exec_status);
}

static bool send_channel_request_status(struct hfoc_ssh_session *session,
                                        const char *request,
                                        bool success, bool want_reply)
{
    uint8_t payload[128];
    struct hfoc_ssh_writer writer;

    if (!want_reply) {
        return true;
    }
    hfoc_ssh_writer_init(&writer, payload, sizeof(payload));
    (void)hfoc_ssh_write_u8(&writer, success ? SSH_MSG_CHANNEL_SUCCESS
                                        : SSH_MSG_CHANNEL_FAILURE);
    (void)hfoc_ssh_write_u32(&writer, session->client_channel);
    if (writer.failed || !queue_packet(session, payload, writer.length)) {
        return false;
    }
    (void)request;
    return true;
}

/* The native TTY stays raw. Validate the mode framing before ignoring it. */
static bool tty_modes_valid(const uint8_t *modes, uint32_t length)
{
    uint32_t offset = 0U;

    if (length == 0U) {
        return true;
    }
    while (offset < length) {
        const uint8_t opcode = modes[offset++];

        if (opcode == 0U) {
            return offset == length;
        }
        if (opcode >= 160U) {
            return true; /* RFC 4254 §8 stops decoding reserved opcodes. */
        }
        if (length - offset < 4U) {
            return false;
        }
        offset += 4U;
    }
    return false;
}

static bool finish_exec(struct hfoc_ssh_session *session, const uint8_t *command,
                        uint32_t command_length)
{
    uint8_t text[HFOC_SSH_SESSION_COMMAND_MAX + 1U];
    size_t output_length = 0U;
    int status;

    if (command_length > HFOC_SSH_SESSION_COMMAND_MAX) {
        return send_disconnect(session, 2U, "command-too-long");
    }
    {
        uint32_t index;

        for (index = 0U; index < command_length; ++index) {
            if (command[index] == 0U || command[index] == '\n' ||
                command[index] == '\r') {
                return send_disconnect(session, 2U, "command-invalid");
            }
        }
    }
    memcpy(text, command, command_length);
    text[command_length] = '\0';
    status = session->command(session->command_context, (const char *)text,
                              session->exec_output, sizeof(session->exec_output),
                              &output_length);
    if (status < 0) {
        status = 127;
    }
    if (output_length > sizeof(session->exec_output)) {
        return send_disconnect(session, 2U, "command-output");
    }
    session->exec_length = output_length;
    session->exec_offset = 0U;
    session->exec_status = status;
    session->exit_sent = false;
    return flush_exec_output(session);
}

static bool handle_channel(struct hfoc_ssh_session *session,
                           const uint8_t *payload, size_t payload_length)
{
    struct hfoc_ssh_reader reader;
    uint8_t message;

    hfoc_ssh_reader_init(&reader, payload, payload_length);
    if (!hfoc_ssh_read_u8(&reader, &message)) {
        return send_disconnect(session, 2U, "channel-parse");
    }
    if (message == SSH_MSG_CHANNEL_OPEN) {
        const uint8_t *type;
        uint32_t type_length;
        uint32_t sender;
        uint32_t window;
        uint32_t max_packet;

        if (session->channel_open ||
            !hfoc_ssh_read_string(&reader, &type, &type_length) ||
            !hfoc_ssh_read_u32(&reader, &sender) ||
            !hfoc_ssh_read_u32(&reader, &window) ||
            !hfoc_ssh_read_u32(&reader, &max_packet) ||
            !hfoc_ssh_reader_done(&reader)) {
            return send_disconnect(session, 2U, "channel-open-parse");
        }
        if (type_length != 7U || memcmp(type, "session", 7U) != 0) {
            uint8_t reply[128];
            struct hfoc_ssh_writer writer;

            hfoc_ssh_writer_init(&writer, reply, sizeof(reply));
            (void)hfoc_ssh_write_u8(&writer, SSH_MSG_CHANNEL_OPEN_FAILURE);
            (void)hfoc_ssh_write_u32(&writer, sender);
            (void)hfoc_ssh_write_u32(&writer, 3U);
            (void)hfoc_ssh_write_cstring(&writer, "unsupported channel type");
            (void)hfoc_ssh_write_cstring(&writer, "");
            if (writer.failed ||
                !queue_packet(session, reply, writer.length)) {
                return false;
            }
            return true;
        }
        /*
         * ITEM-603: a zero maximum packet size is not a usable channel and a
         * value above our packet bound is clamped, not trusted.  The peer's
         * own maximum still caps every outgoing CHANNEL_DATA chunk.
         */
        if (max_packet == 0U) {
            return send_disconnect(session, 2U, "channel-max-packet");
        }
        session->client_channel = sender;
        session->server_channel = 0U;
        session->client_window = window;
        session->client_max_packet =
            max_packet > HFOC_SSH_SESSION_PACKET_MAX
                ? HFOC_SSH_SESSION_PACKET_MAX
                : max_packet;
        session->server_window = HFOC_SSH_SESSION_SERVER_WINDOW;
        session->channel_open = true;
        {
            uint8_t reply[64];
            struct hfoc_ssh_writer writer;

            hfoc_ssh_writer_init(&writer, reply, sizeof(reply));
            (void)hfoc_ssh_write_u8(&writer, SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
            (void)hfoc_ssh_write_u32(&writer, sender);
            (void)hfoc_ssh_write_u32(&writer, 0U);
            (void)hfoc_ssh_write_u32(&writer,
                                     HFOC_SSH_SESSION_SERVER_WINDOW);
            (void)hfoc_ssh_write_u32(&writer, HFOC_SSH_SESSION_PACKET_MAX);
            if (writer.failed ||
                !queue_packet(session, reply, writer.length)) {
                return false;
            }
        }
        return true;
    }
    if (!session->channel_open) {
        return send_disconnect(session, 2U, "channel-not-open");
    }
    if (message == SSH_MSG_CHANNEL_REQUEST) {
        uint32_t recipient;
        const uint8_t *request;
        uint32_t request_length;
        bool want_reply;

        if (!hfoc_ssh_read_u32(&reader, &recipient) ||
            !hfoc_ssh_read_string(&reader, &request, &request_length) ||
            !hfoc_ssh_read_bool(&reader, &want_reply)) {
            return send_disconnect(session, 2U, "channel-request-parse");
        }
        if (recipient != session->server_channel) {
            return send_disconnect(session, 2U, "channel-recipient");
        }
        if (request_length == 4U && memcmp(request, "exec", 4U) == 0) {
            const uint8_t *command;
            uint32_t command_length;
            uint8_t command_copy[HFOC_SSH_SESSION_COMMAND_MAX];

            if (session->exec_started || session->shell_started ||
                !hfoc_ssh_read_string(&reader, &command, &command_length) ||
                !hfoc_ssh_reader_done(&reader) ||
                command_length > sizeof(command_copy)) {
                return send_disconnect(session, 2U, "exec-parse");
            }
            /* queue_packet() reuses the receive scratch, so copy before it. */
            memcpy(command_copy, command, command_length);
            session->exec_started = true;
            if (!send_channel_request_status(session, "exec", true,
                                             want_reply)) {
                return false;
            }
            return finish_exec(session, command_copy, command_length);
        }
        if (request_length == 7U && memcmp(request, "pty-req", 7U) == 0) {
            const uint8_t *term;
            const uint8_t *modes;
            uint32_t term_length;
            uint32_t modes_length;
            uint32_t columns;
            uint32_t rows;
            uint32_t width;
            uint32_t height;

            if (session->shell_started || session->exec_started) {
                session->reason = "pty-req-state";
                return send_channel_request_status(session, "pty-req", false,
                                                   want_reply);
            }
            if (!hfoc_ssh_read_string(&reader, &term, &term_length) ||
                !hfoc_ssh_read_u32(&reader, &columns) ||
                !hfoc_ssh_read_u32(&reader, &rows) ||
                !hfoc_ssh_read_u32(&reader, &width) ||
                !hfoc_ssh_read_u32(&reader, &height) ||
                !hfoc_ssh_read_string(&reader, &modes, &modes_length) ||
                !hfoc_ssh_reader_done(&reader)) {
                return send_disconnect(session, 2U, "pty-req-parse");
            }
            if (term_length > HFOC_SSH_SESSION_PTY_TERM_MAX ||
                columns > 4096U || rows > 4096U ||
                width > 1048576U || height > 1048576U ||
                modes_length > HFOC_SSH_SESSION_PTY_MODES_MAX) {
                session->reason = "pty-req-bounds";
                return send_channel_request_status(session, "pty-req", false,
                                                   want_reply);
            }
            if (!tty_modes_valid(modes, modes_length)) {
                session->reason = "pty-req-modes";
                return send_channel_request_status(session, "pty-req", false,
                                                   want_reply);
            }
            for (uint32_t index = 0U; index < term_length; ++index) {
                if (term[index] < 0x20U || term[index] > 0x7eU) {
                    session->reason = "pty-req-term";
                    return send_channel_request_status(
                        session, "pty-req", false, want_reply);
                }
            }
            memcpy(session->pty_term, term, term_length);
            session->pty_term[term_length] = '\0';
            session->pty_columns = columns != 0U ? columns : 80U;
            session->pty_rows = rows != 0U ? rows : 24U;
            session->pty_requested = true;
            return send_channel_request_status(session, "pty-req", true,
                                               want_reply);
        }
        if (request_length == 5U && memcmp(request, "shell", 5U) == 0) {
            char term[HFOC_SSH_SESSION_PTY_TERM_MAX + 1U];
            uint32_t columns = session->pty_requested
                                   ? session->pty_columns
                                   : 80U;
            uint32_t rows = session->pty_requested ? session->pty_rows : 24U;

            if (!hfoc_ssh_reader_done(&reader) || session->exec_started ||
                session->shell_started || session->shell_ops == NULL ||
                session->shell_ops->start == NULL) {
                return send_channel_request_status(session, "shell", false,
                                                   want_reply);
            }
            if (session->pty_requested) {
                memcpy(term, session->pty_term, sizeof(term));
            } else {
                memcpy(term, "dumb", 5U);
            }
            if (!session->shell_ops->start(session->shell_context, term,
                                           columns, rows)) {
                return send_channel_request_status(session, "shell", false,
                                                   want_reply);
            }
            session->shell_started = true;
            session->shell_eof = false;
            if (!send_channel_request_status(session, "shell", true,
                                             want_reply)) {
                return false;
            }
            return flush_exec_output(session);
        }
        if (request_length == 13U &&
            memcmp(request, "window-change", 13U) == 0) {
            uint32_t columns;
            uint32_t rows;
            uint32_t width;
            uint32_t height;

            if (!hfoc_ssh_read_u32(&reader, &columns) ||
                !hfoc_ssh_read_u32(&reader, &rows) ||
                !hfoc_ssh_read_u32(&reader, &width) ||
                !hfoc_ssh_read_u32(&reader, &height) ||
                !hfoc_ssh_reader_done(&reader) ||
                columns > 4096U || rows > 4096U ||
                width > 1048576U || height > 1048576U) {
                return send_disconnect(session, 2U, "window-change-parse");
            }
            session->pty_columns = columns != 0U ? columns : 80U;
            session->pty_rows = rows != 0U ? rows : 24U;
            if (session->shell_started && session->shell_ops != NULL &&
                session->shell_ops->resize != NULL &&
                !session->shell_ops->resize(session->shell_context, columns,
                                            rows)) {
                return send_channel_request_status(
                    session, "window-change", false, want_reply);
            }
            return send_channel_request_status(session, "window-change", true,
                                               want_reply);
        }
        if (request_length == 6U && memcmp(request, "signal", 6U) == 0) {
            const uint8_t *name;
            uint32_t name_length;
            bool allowed = false;

            if (!hfoc_ssh_read_string(&reader, &name, &name_length) ||
                !hfoc_ssh_reader_done(&reader) || name_length == 0U ||
                name_length > 16U) {
                return send_disconnect(session, 2U, "signal-parse");
            }
            for (uint32_t index = 0U; index < name_length; ++index) {
                if (name[index] < 0x20U || name[index] > 0x7eU) {
                    return send_disconnect(session, 2U, "signal-name");
                }
            }
            if ((name_length == 3U && memcmp(name, "INT", 3U) == 0) ||
                (name_length == 3U && memcmp(name, "HUP", 3U) == 0) ||
                (name_length == 4U && memcmp(name, "TERM", 4U) == 0) ||
                (name_length == 4U && memcmp(name, "QUIT", 4U) == 0)) {
                allowed = true;
            }
            if (!allowed || !session->shell_started ||
                session->shell_ops == NULL ||
                session->shell_ops->signal == NULL ||
                !session->shell_ops->signal(session->shell_context,
                                             (const char *)name,
                                             name_length)) {
                return send_channel_request_status(session, "signal", false,
                                                   want_reply);
            }
            return send_channel_request_status(session, "signal", true,
                                               want_reply);
        }
        if (request_length == 3U && memcmp(request, "env", 3U) == 0) {
            const uint8_t *name;
            const uint8_t *value;
            uint32_t name_length;
            uint32_t value_length;

            if (!hfoc_ssh_read_string(&reader, &name, &name_length) ||
                !hfoc_ssh_read_string(&reader, &value, &value_length) ||
                !hfoc_ssh_reader_done(&reader) ||
                name_length > 256U || value_length > 1024U) {
                return send_disconnect(session, 2U, "env-parse");
            }
            /* The owner shell has its own fixed environment; accept the
               client's env requests so OpenSSH proceeds to pty/shell. */
            return send_channel_request_status(session, "env", true,
                                               want_reply);
        }
        if (request_length == 9U &&
            memcmp(request, "subsystem", 9U) == 0) {
            const uint8_t *name;
            uint32_t name_length;

            if (!hfoc_ssh_read_string(&reader, &name, &name_length) ||
                !hfoc_ssh_reader_done(&reader) || name_length > 256U) {
                return send_disconnect(session, 2U, "subsystem-parse");
            }
            return send_channel_request_status(session, "subsystem", false,
                                               want_reply);
        }
        if (!hfoc_ssh_reader_done(&reader)) {
            return send_disconnect(session, 2U, "channel-request-tail");
        }
        /* forwarding and other unknown requests remain explicit refusals. */
        return send_channel_request_status(session, "refused", false,
                                           want_reply);
    }
    if (message == SSH_MSG_CHANNEL_DATA) {
        uint32_t recipient;
        const uint8_t *data;
        uint32_t data_length;

        if (!hfoc_ssh_read_u32(&reader, &recipient) ||
            !hfoc_ssh_read_string(&reader, &data, &data_length) ||
            !hfoc_ssh_reader_done(&reader)) {
            return send_disconnect(session, 2U, "channel-data-parse");
        }
        if (recipient != session->server_channel) {
            return send_disconnect(session, 2U, "channel-recipient");
        }
        if (!session->shell_started) {
            (void)data;
            (void)data_length;
            return true;
        }
        if (session->shell_eof || data_length > session->server_window) {
            return send_disconnect(session, 2U, "shell-input-window");
        }
        session->server_window -= data_length;
        if (session->shell_ops == NULL ||
            session->shell_ops->input == NULL) {
            return send_disconnect(session, 2U, "shell-input-unavailable");
        }
        {
            size_t accepted = 0U;

            if (!session->shell_ops->input(session->shell_context, data,
                                           data_length, &accepted) ||
                accepted != data_length) {
                return send_disconnect(session, 2U, "shell-input-overflow");
            }
        }
        return true;
    }
    if (message == SSH_MSG_CHANNEL_EOF) {
        uint32_t recipient;

        if (!hfoc_ssh_read_u32(&reader, &recipient) || !hfoc_ssh_reader_done(&reader)) {
            return send_disconnect(session, 2U, "channel-eof-parse");
        }
        if (recipient != session->server_channel) {
            return send_disconnect(session, 2U, "channel-recipient");
        }
        if (session->shell_started && !session->shell_eof) {
            session->shell_eof = true;
            if (session->shell_ops != NULL &&
                session->shell_ops->eof != NULL) {
                session->shell_ops->eof(session->shell_context);
            }
        }
        return true;
    }
    if (message == SSH_MSG_CHANNEL_CLOSE) {
        uint32_t recipient;
        uint8_t reply[16];
        struct hfoc_ssh_writer writer;

        if (!hfoc_ssh_read_u32(&reader, &recipient) || !hfoc_ssh_reader_done(&reader)) {
            return send_disconnect(session, 2U, "channel-close-parse");
        }
        if (recipient != session->server_channel) {
            return send_disconnect(session, 2U, "channel-recipient");
        }
        if (session->shell_started && session->shell_ops != NULL &&
            session->shell_ops->hangup != NULL) {
            session->shell_ops->hangup(session->shell_context);
        }
        if (!session->close_sent) {
            hfoc_ssh_writer_init(&writer, reply, sizeof(reply));
            (void)hfoc_ssh_write_u8(&writer, SSH_MSG_CHANNEL_CLOSE);
            (void)hfoc_ssh_write_u32(&writer, session->client_channel);
            if (writer.failed ||
                !queue_packet(session, reply, writer.length)) {
                return false;
            }
            session->close_sent = true;
        }
        session->state = HFOC_SSH_SESSION_STATE_DONE;
        return true;
    }
    if (message == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
        uint32_t recipient;
        uint32_t increment;

        if (!hfoc_ssh_read_u32(&reader, &recipient) ||
            !hfoc_ssh_read_u32(&reader, &increment) ||
            !hfoc_ssh_reader_done(&reader)) {
            return send_disconnect(session, 2U, "window-adjust-parse");
        }
        if (recipient != session->server_channel) {
            return send_disconnect(session, 2U, "channel-recipient");
        }
        if (increment > UINT32_MAX - session->client_window) {
            session->client_window = UINT32_MAX;
        } else {
            session->client_window += increment;
        }
        return flush_exec_output(session);
    }
    return send_disconnect(session, 2U, "unsupported-channel-message");
}

static bool dispatch_payload(struct hfoc_ssh_session *session,
                             const uint8_t *payload, size_t payload_length)
{
    uint8_t message;

    if (payload_length == 0U) {
        return send_disconnect(session, 2U, "empty-payload");
    }
    message = payload[0];
    session->last_message = message;
    if (message == SSH_MSG_DISCONNECT) {
        session->state = HFOC_SSH_SESSION_STATE_DONE;
        return true;
    }
    if (message == SSH_MSG_IGNORE || message == SSH_MSG_DEBUG) {
        if (session->strict_kex &&
            session->state != HFOC_SSH_SESSION_STATE_CHANNEL) {
            return send_disconnect(session, 2U, "strict-kex-ignore");
        }
        return true;
    }
    if (message == SSH_MSG_UNIMPLEMENTED) {
        return true;
    }
    if (message == SSH_MSG_KEXINIT) {
        return parse_kexinit(session, payload, payload_length);
    }
    if (message == SSH_MSG_KEX_ECDH_INIT) {
        return handle_kex_ecdh(session, payload, payload_length);
    }
    if (message == SSH_MSG_NEWKEYS) {
        if (session->state != HFOC_SSH_SESSION_STATE_NEWKEYS ||
            payload_length != 1U) {
            return send_disconnect(session, 2U, "unexpected-newkeys");
        }
        if (!derive_and_init_keys(session, false, true)) {
            return send_disconnect(session, 2U, "kdf-rx");
        }
        session->encrypted_rx = true;
        if (session->strict_kex) {
            session->sequence_rx = 0U;
        }
        session->state = HFOC_SSH_SESSION_STATE_SERVICE;
        return true;
    }
    if (message == SSH_MSG_SERVICE_REQUEST) {
        struct hfoc_ssh_reader reader;
        const uint8_t *service;
        uint32_t service_length;
        uint8_t reply[64];
        struct hfoc_ssh_writer writer;

        if (session->state != HFOC_SSH_SESSION_STATE_SERVICE) {
            return send_disconnect(session, 2U, "unexpected-service");
        }
        hfoc_ssh_reader_init(&reader, payload, payload_length);
        {
            uint8_t type;
            if (!hfoc_ssh_read_u8(&reader, &type) ||
                !hfoc_ssh_read_string(&reader, &service, &service_length) ||
                !hfoc_ssh_reader_done(&reader) || service_length != 12U ||
                memcmp(service, "ssh-userauth", 12U) != 0) {
                return send_disconnect(session, 2U, "service-request");
            }
        }
        hfoc_ssh_writer_init(&writer, reply, sizeof(reply));
        (void)hfoc_ssh_write_u8(&writer, SSH_MSG_SERVICE_ACCEPT);
        (void)hfoc_ssh_write_string(&writer, service, service_length);
        if (writer.failed || !queue_packet(session, reply, writer.length)) {
            return false;
        }
        session->state = HFOC_SSH_SESSION_STATE_AUTH;
        return true;
    }
    if (message == SSH_MSG_USERAUTH_REQUEST) {
        return handle_userauth(session, payload, payload_length);
    }
    if (message == SSH_MSG_GLOBAL_REQUEST) {
        uint8_t reply[16];
        struct hfoc_ssh_writer writer;

        hfoc_ssh_writer_init(&writer, reply, sizeof(reply));
        (void)hfoc_ssh_write_u8(&writer, SSH_MSG_REQUEST_FAILURE);
        if (writer.failed || !queue_packet(session, reply, writer.length)) {
            return false;
        }
        return true;
    }
    if (message >= SSH_MSG_CHANNEL_OPEN &&
        message <= SSH_MSG_CHANNEL_FAILURE) {
        if (session->state != HFOC_SSH_SESSION_STATE_CHANNEL &&
            session->state != HFOC_SSH_SESSION_STATE_CLOSING) {
            return send_disconnect(session, 2U, "channel-state");
        }
        return handle_channel(session, payload, payload_length);
    }
    return send_disconnect(session, 2U, "unexpected-message");
}

static bool process_packet(struct hfoc_ssh_session *session,
                           const uint8_t *payload, size_t payload_length)
{
    if (session->state < HFOC_SSH_SESSION_STATE_CHANNEL &&
        ++session->preauth_packets > HFOC_SSH_SESSION_PREAUTH_LIMIT) {
        return send_disconnect(session, 2U, "too-many-pre-auth-messages");
    }
    ++session->sequence_rx;
    session->bytes_rx += payload_length;
    return dispatch_payload(session, payload, payload_length);
}

static enum hfoc_ssh_session_result process_input(struct hfoc_ssh_session *session)
{
    while (session->input_length != 0U) {
        if (session->state == HFOC_SSH_SESSION_STATE_IDENT) {
            size_t index;
            size_t line_end = 0U;
            bool found = false;

            for (index = 0U; index < session->input_length; ++index) {
                if (session->input[index] == '\n') {
                    line_end = index;
                    found = true;
                    break;
                }
                if (index >= HFOC_SSH_SESSION_VERSION_MAX) {
                    session->reason = "ident-too-long";
                    return HFOC_SSH_SESSION_PROTOCOL_ERROR;
                }
            }
            if (!found) {
                return HFOC_SSH_SESSION_NEED_MORE;
            }
            {
                size_t line_length = line_end;
                if (line_length != 0U &&
                    session->input[line_length - 1U] == '\r') {
                    --line_length;
                }
                if (session->preauth_packets >=
                        HFOC_SSH_SESSION_PREAUTH_LIMIT) {
                    session->reason = "too-many-pre-auth-messages";
                    return HFOC_SSH_SESSION_PROTOCOL_ERROR;
                }
                ++session->preauth_packets;
                if (line_length >= 4U &&
                    memcmp(session->input, "SSH-", 4U) == 0) {
                    memcpy(session->client_version, session->input,
                           line_length);
                    session->client_version_length = line_length;
                    memmove(session->input,
                            session->input + line_end + 1U,
                            session->input_length - line_end - 1U);
                    session->input_length -= line_end + 1U;
                    if (!build_kexinit(session)) {
                        return HFOC_SSH_SESSION_PROTOCOL_ERROR;
                    }
                    continue;
                }
                /* Preamble line; bounded and ignored. */
                memmove(session->input, session->input + line_end + 1U,
                        session->input_length - line_end - 1U);
                session->input_length -= line_end + 1U;
                continue;
            }
        }
        if (!session->encrypted_rx) {
            uint32_t length;
            size_t total;

            if (session->input_length < 4U) {
                return HFOC_SSH_SESSION_NEED_MORE;
            }
            length = ((uint32_t)session->input[0] << 24) |
                     ((uint32_t)session->input[1] << 16) |
                     ((uint32_t)session->input[2] << 8) |
                     (uint32_t)session->input[3];
            if (length < 5U || length > HFOC_SSH_SESSION_PACKET_MAX) {
                session->reason = "packet-length";
                return HFOC_SSH_SESSION_PROTOCOL_ERROR;
            }
            total = 4U + length;
            if (session->input_length < total) {
                return HFOC_SSH_SESSION_NEED_MORE;
            }
            {
                uint8_t padding_length = session->input[4];
                size_t payload_length;

                if (padding_length < 4U || padding_length >= length) {
                    session->reason = "packet-padding";
                    return HFOC_SSH_SESSION_PROTOCOL_ERROR;
                }
                payload_length = length - 1U - padding_length;
                if (payload_length == 0U) {
                    session->reason = "packet-payload";
                    return HFOC_SSH_SESSION_PROTOCOL_ERROR;
                }
                if (!process_packet(session, session->input + 5U,
                                    payload_length)) {
                    return HFOC_SSH_SESSION_PROTOCOL_ERROR;
                }
            }
            memmove(session->input, session->input + total,
                    session->input_length - total);
            session->input_length -= total;
            continue;
        }
        {
            uint32_t length;
            size_t total;

            if (session->input_length < 4U) {
                return HFOC_SSH_SESSION_NEED_MORE;
            }
            length = ((uint32_t)session->input[0] << 24) |
                     ((uint32_t)session->input[1] << 16) |
                     ((uint32_t)session->input[2] << 8) |
                     (uint32_t)session->input[3];
            /*
             * AEAD packet_length is a multiple of the 16-byte block and
             * includes the padding_length byte, the payload and the padding.
             * A minimum CHANNEL_EOF packet is therefore exactly 16, not 17;
             * rejecting 16 turned a normal client EOF into a disconnect.
             */
            if (length < 16U ||
                length > HFOC_SSH_SESSION_INPUT_MAX - 4U - 16U) {
                session->reason = "packet-length";
                return HFOC_SSH_SESSION_PROTOCOL_ERROR;
            }
            total = 4U + length + 16U;
            if (session->input_length < total) {
                return HFOC_SSH_SESSION_NEED_MORE;
            }
            {
                uint8_t *plain = session->scratch_a;
                uint8_t padding_length;
                size_t payload_length;

                if (!hfoc_ssh_crypto_open(&session->crypto, session->input,
                                     session->input + 4U, length,
                                     session->input + 4U + length, plain)) {
                    session->reason = "packet-auth";
                    return HFOC_SSH_SESSION_CRYPTO_ERROR;
                }
                padding_length = plain[0];
                if (padding_length < 4U || padding_length >= length - 1U) {
                    session->reason = "packet-padding";
                    return HFOC_SSH_SESSION_PROTOCOL_ERROR;
                }
                payload_length = length - 1U - padding_length;
                if (payload_length == 0U) {
                    session->reason = "packet-payload";
                    return HFOC_SSH_SESSION_PROTOCOL_ERROR;
                }
                if (!process_packet(session, plain + 1U, payload_length)) {
                    return HFOC_SSH_SESSION_PROTOCOL_ERROR;
                }
            }
            memmove(session->input, session->input + total,
                    session->input_length - total);
            session->input_length -= total;
        }
    }
    if (session->state == HFOC_SSH_SESSION_STATE_DONE) {
        return HFOC_SSH_SESSION_CLOSED;
    }
    if (session->state == HFOC_SSH_SESSION_STATE_FAILED) {
        return HFOC_SSH_SESSION_PROTOCOL_ERROR;
    }
    return HFOC_SSH_SESSION_NEED_MORE;
}

void hfoc_ssh_session_start(struct hfoc_ssh_session *session, uint32_t now_ms)
{
    if (session == NULL || session->started) {
        return;
    }
    session->started = true;
    session->started_ms = now_ms;
    session->last_activity_ms = now_ms;
}

static bool hfoc_ssh_session_fail(struct hfoc_ssh_session *session,
                                  const char *reason)
{
    session->state = HFOC_SSH_SESSION_STATE_FAILED;
    session->reason = reason;
    return false;
}

bool hfoc_ssh_session_tick(struct hfoc_ssh_session *session, uint32_t now_ms)
{
    uint32_t age;
    uint32_t idle;

    if (session == NULL) {
        return false;
    }
    if (session->state == HFOC_SSH_SESSION_STATE_DONE ||
        session->state == HFOC_SSH_SESSION_STATE_FAILED) {
        return false;
    }
    if (!session->started) {
        hfoc_ssh_session_start(session, now_ms);
    }
    age = (uint32_t)(now_ms - session->started_ms);
    idle = (uint32_t)(now_ms - session->last_activity_ms);
    if (age >= HFOC_SSH_SESSION_ABSOLUTE_TIMEOUT_MS) {
        return hfoc_ssh_session_fail(session, "absolute-timeout");
    }
    /*
     * v1 closes cleanly at the rekey limit instead of continuing with the
     * same key material.  The --rekey limit is a close, not a silent reset.
     */
    if (age >= HFOC_SSH_SESSION_REKEY_TIME_MS ||
        session->bytes_tx >= HFOC_SSH_SESSION_REKEY_BYTES ||
        session->bytes_rx >= HFOC_SSH_SESSION_REKEY_BYTES) {
        session->reason = "rekey-limit";
        session->state = HFOC_SSH_SESSION_STATE_DONE;
        return false;
    }
    if (session->state <= HFOC_SSH_SESSION_STATE_NEWKEYS &&
        age >= HFOC_SSH_SESSION_HANDSHAKE_TIMEOUT_MS) {
        return hfoc_ssh_session_fail(session, "handshake-timeout");
    }
    if ((session->state == HFOC_SSH_SESSION_STATE_SERVICE ||
         session->state == HFOC_SSH_SESSION_STATE_AUTH) &&
        age >= HFOC_SSH_SESSION_AUTH_TIMEOUT_MS) {
        return hfoc_ssh_session_fail(session, "auth-timeout");
    }
    if (session->state >= HFOC_SSH_SESSION_STATE_CHANNEL &&
        idle >= HFOC_SSH_SESSION_IDLE_TIMEOUT_MS) {
        return hfoc_ssh_session_fail(session, "idle-timeout");
    }
    return true;
}

bool hfoc_ssh_session_init(struct hfoc_ssh_session *session, hfoc_ssh_rng_fn rng,
                      void *rng_context, hfoc_ssh_command_fn command,
                      void *command_context)
{
    const char *version = ssh_server_version;

    memset(session, 0, sizeof(*session));
    session->rng = rng;
    session->rng_context = rng_context;
    session->command = command;
    session->command_context = command_context;
    session->state = HFOC_SSH_SESSION_STATE_IDENT;
    session->server_window = HFOC_SSH_SESSION_SERVER_WINDOW;
    session->started = false;
    session->started_ms = 0U;
    session->last_activity_ms = 0U;
    session->server_version_length = strlen(version);
    if (session->server_version_length >= sizeof(session->server_version)) {
        return false;
    }
    memcpy(session->server_version, version, session->server_version_length);
    if (!hfoc_ssh_crypto_init(&session->crypto)) {
        return false;
    }
    if (!out_append(session, version, session->server_version_length) ||
        !out_append(session, "\r\n", 2U)) {
        return false;
    }
    return true;
}

void hfoc_ssh_session_set_shell(struct hfoc_ssh_session *session,
                                const struct hfoc_ssh_shell_ops *ops,
                                void *context)
{
    if (session == NULL) {
        return;
    }
    session->shell_ops = ops;
    session->shell_context = context;
}

void hfoc_ssh_session_free(struct hfoc_ssh_session *session)
{
    hfoc_ssh_crypto_free(&session->crypto);
    memset(session, 0, sizeof(*session));
}

enum hfoc_ssh_session_result hfoc_ssh_session_feed(
    struct hfoc_ssh_session *session, const uint8_t *data, size_t length,
    uint32_t now_ms)
{
    if (session == NULL || (data == NULL && length != 0U)) {
        return HFOC_SSH_SESSION_PROTOCOL_ERROR;
    }
    if (!hfoc_ssh_session_tick(session, now_ms)) {
        return session->state == HFOC_SSH_SESSION_STATE_DONE
            ? HFOC_SSH_SESSION_CLOSED
            : HFOC_SSH_SESSION_PROTOCOL_ERROR;
    }
    session->last_activity_ms = now_ms;
    while (length != 0U) {
        size_t room = sizeof(session->input) - session->input_length;
        size_t chunk = length < room ? length : room;

        if (chunk == 0U) {
            session->reason = "input-buffer-full";
            return HFOC_SSH_SESSION_PROTOCOL_ERROR;
        }
        memcpy(session->input + session->input_length, data, chunk);
        session->input_length += chunk;
        data += chunk;
        length -= chunk;
        {
            enum hfoc_ssh_session_result result = process_input(session);
            if (result != HFOC_SSH_SESSION_NEED_MORE) {
                return result;
            }
        }
    }
    return HFOC_SSH_SESSION_NEED_MORE;
}

size_t hfoc_ssh_session_take_output(struct hfoc_ssh_session *session, uint8_t *data,
                               size_t capacity)
{
    size_t take = session->output_length < capacity ? session->output_length
                                                    : capacity;

    if (take != 0U) {
        memcpy(data, session->output, take);
        memmove(session->output, session->output + take,
                session->output_length - take);
        session->output_length -= take;
    }
    return take;
}

bool hfoc_ssh_session_flush_output(struct hfoc_ssh_session *session)
{
    if (session == NULL) {
        return false;
    }
    return flush_exec_output(session);
}

bool hfoc_ssh_session_done(const struct hfoc_ssh_session *session)
{
    return session->state == HFOC_SSH_SESSION_STATE_DONE ||
           session->state == HFOC_SSH_SESSION_STATE_FAILED;
}

const char *hfoc_ssh_session_reason(const struct hfoc_ssh_session *session)
{
    return session->reason != NULL ? session->reason : "none";
}

#else
typedef int hfoc_ssh_transport_translation_unit_guard;
#endif
