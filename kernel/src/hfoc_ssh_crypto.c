#ifdef HFOC_PRODUCTION_TLS

#include "hfoc_ssh_crypto.h"

#include "hfoc_ssh_wire.h"

#include <string.h>

#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

static const char ssh_host_algorithm[] = "ecdsa-sha2-nistp256";
static const char ssh_curve_name[] = "nistp256";

/*
 * ITEM-607: wipe stack copies of ECDH shared secrets, derived keys and
 * signature nonces.  The loop is volatile so an optimising compiler cannot
 * treat the buffer as dead and drop the store.
 */
static void ssh_secret_zero(void *data, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)data;

    while (length != 0U) {
        *bytes++ = 0U;
        --length;
    }
}

/*
 * ITEM-616: hash the exchange-hash fields incrementally.  The client KEXINIT
 * is bounded at 4096 bytes, so materialising v_c || v_s || i_c || i_s ||
 * host_blob || q_c || q_s || shared into one fixed array turned a standard
 * OpenSSH MAC name-list into a named "exchange-hash" disconnect.  These
 * helpers mirror hfoc_ssh_write_string()/hfoc_ssh_write_mpint() byte for byte
 * because the signed transcript must not change.
 */
static bool ssh_hash_update(mbedtls_sha256_context *context, const void *data,
                            size_t length)
{
    if (length == 0U) {
        return true;
    }
    if (data == NULL) {
        return false;
    }
    return mbedtls_sha256_update(context, data, length) == 0;
}

static bool ssh_hash_u32(mbedtls_sha256_context *context, uint32_t value)
{
    uint8_t bytes[4];

    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
    return ssh_hash_update(context, bytes, sizeof(bytes));
}

static bool ssh_hash_string(mbedtls_sha256_context *context, const void *data,
                            size_t length)
{
    if (length > UINT32_MAX) {
        return false;
    }
    return ssh_hash_u32(context, (uint32_t)length) &&
           ssh_hash_update(context, data, length);
}

static bool ssh_hash_mpint(mbedtls_sha256_context *context, const uint8_t *data,
                           size_t length)
{
    uint8_t zero = 0U;

    /* Trim leading zero bytes but keep one value byte for zero. */
    while (length > 1U && data[0] == 0U) {
        ++data;
        --length;
    }
    if (length == 0U) {
        return ssh_hash_string(context, &zero, 0U);
    }
    if ((data[0] & 0x80U) != 0U) {
        uint8_t prefix = 0U;

        return ssh_hash_u32(context, (uint32_t)(length + 1U)) &&
               ssh_hash_update(context, &prefix, 1U) &&
               ssh_hash_update(context, data, length);
    }
    return ssh_hash_string(context, data, length);
}

static bool append_host_blob(struct hfoc_ssh_crypto *crypto)
{
    struct hfoc_ssh_writer writer;
    uint8_t point[65];
    size_t point_length = 0U;

    {
        int rc = mbedtls_ecp_point_write_binary(
            &crypto->host.MBEDTLS_PRIVATE(grp),
            &crypto->host.MBEDTLS_PRIVATE(Q), MBEDTLS_ECP_PF_UNCOMPRESSED,
            &point_length, point, sizeof(point));
        if (rc != 0 || point_length != sizeof(point)) {
            return false;
        }
    }
    hfoc_ssh_writer_init(&writer, crypto->host_blob, sizeof(crypto->host_blob));
    (void)hfoc_ssh_write_cstring(&writer, ssh_host_algorithm);
    (void)hfoc_ssh_write_cstring(&writer, ssh_curve_name);
    (void)hfoc_ssh_write_string(&writer, point, point_length);
    if (writer.failed) {
        return false;
    }
    crypto->host_blob_length = writer.length;
    if (mbedtls_sha256(crypto->host_blob, crypto->host_blob_length,
                       crypto->host_fingerprint, 0) != 0) {
        return false;
    }
    return true;
}

bool hfoc_ssh_crypto_init(struct hfoc_ssh_crypto *crypto)
{
    memset(crypto, 0, sizeof(*crypto));
    mbedtls_ecp_group_init(&crypto->group);
    mbedtls_ecdsa_init(&crypto->host);
    mbedtls_mpi_init(&crypto->ephemeral_secret);
    mbedtls_ecp_point_init(&crypto->ephemeral_public);
    mbedtls_mpi_init(&crypto->shared);
    mbedtls_gcm_init(&crypto->tx);
    mbedtls_gcm_init(&crypto->rx);
    return mbedtls_ecp_group_load(&crypto->group,
                                  MBEDTLS_ECP_DP_SECP256R1) == 0;
}

void hfoc_ssh_crypto_free(struct hfoc_ssh_crypto *crypto)
{
    mbedtls_gcm_free(&crypto->tx);
    mbedtls_gcm_free(&crypto->rx);
    mbedtls_mpi_free(&crypto->shared);
    mbedtls_ecp_point_free(&crypto->ephemeral_public);
    mbedtls_mpi_free(&crypto->ephemeral_secret);
    mbedtls_ecdsa_free(&crypto->host);
    mbedtls_ecp_group_free(&crypto->group);
    memset(crypto, 0, sizeof(*crypto));
}

bool hfoc_ssh_crypto_host_key_load(struct hfoc_ssh_crypto *crypto,
                              const uint8_t scalar[32], hfoc_ssh_rng_fn rng,
                              void *rng_context)
{
    int rc = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &crypto->host,
                                  scalar, 32);

    if (rc == 0) {
        /* mbedtls_ecp_read_key only imports the private scalar. */
        rc = mbedtls_ecp_mul(&crypto->host.MBEDTLS_PRIVATE(grp),
                             &crypto->host.MBEDTLS_PRIVATE(Q),
                             &crypto->host.MBEDTLS_PRIVATE(d),
                             &crypto->host.MBEDTLS_PRIVATE(grp).G, rng,
                             rng_context);
    }
    if (rc != 0) {
        return false;
    }
    crypto->host_loaded = append_host_blob(crypto);
    return crypto->host_loaded;
}

bool hfoc_ssh_crypto_host_key_generate(struct hfoc_ssh_crypto *crypto, hfoc_ssh_rng_fn rng,
                                  void *rng_context)
{
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &crypto->host, rng,
                            rng_context) != 0) {
        return false;
    }
    crypto->host_loaded = append_host_blob(crypto);
    return crypto->host_loaded;
}

const uint8_t *hfoc_ssh_crypto_host_blob(const struct hfoc_ssh_crypto *crypto,
                                    size_t *length)
{
    *length = crypto->host_blob_length;
    return crypto->host_blob;
}

const uint8_t *hfoc_ssh_crypto_host_fingerprint(const struct hfoc_ssh_crypto *crypto)
{
    return crypto->host_fingerprint;
}

bool hfoc_ssh_crypto_kex_start(struct hfoc_ssh_crypto *crypto, hfoc_ssh_rng_fn rng,
                          void *rng_context)
{
    if (mbedtls_ecdh_gen_public(&crypto->group, &crypto->ephemeral_secret,
                                &crypto->ephemeral_public, rng,
                                rng_context) != 0) {
        return false;
    }
    crypto->ephemeral_ready = true;
    return true;
}

const uint8_t *hfoc_ssh_crypto_kex_public(const struct hfoc_ssh_crypto *crypto)
{
    static uint8_t serialized[65];
    size_t length = 0U;

    if (!crypto->ephemeral_ready ||
        mbedtls_ecp_point_write_binary(
            &crypto->group, &crypto->ephemeral_public,
            MBEDTLS_ECP_PF_UNCOMPRESSED, &length, serialized,
            sizeof(serialized)) != 0 ||
        length != sizeof(serialized)) {
        return NULL;
    }
    return serialized;
}

bool hfoc_ssh_crypto_kex_shared(struct hfoc_ssh_crypto *crypto, const uint8_t *peer,
                           size_t peer_length, hfoc_ssh_rng_fn rng,
                           void *rng_context, uint8_t *mpint,
                           size_t *mpint_length)
{
    mbedtls_ecp_point peer_point;
    size_t size;
    bool ok = false;

    mbedtls_ecp_point_init(&peer_point);
    if (mbedtls_ecp_point_read_binary(&crypto->group, &peer_point, peer,
                                      peer_length) != 0) {
        goto done;
    }
    if (mbedtls_ecdh_compute_shared(&crypto->group, &crypto->shared,
                                    &peer_point, &crypto->ephemeral_secret,
                                    rng, rng_context) != 0) {
        goto done;
    }
    size = mbedtls_mpi_size(&crypto->shared);
    if (size > HFOC_SSH_CRYPTO_SHARED_MAX ||
        mbedtls_mpi_write_binary(&crypto->shared, mpint, size) != 0) {
        goto done;
    }
    *mpint_length = size;
    crypto->shared_ready = true;
    ok = true;
done:
    mbedtls_ecp_point_free(&peer_point);
    return ok;
}

bool hfoc_ssh_crypto_exchange_hash(
    struct hfoc_ssh_crypto *crypto, const uint8_t *v_c, size_t v_c_length,
    const uint8_t *v_s, size_t v_s_length, const uint8_t *i_c,
    size_t i_c_length, const uint8_t *i_s, size_t i_s_length,
    const uint8_t *q_c, size_t q_c_length, uint8_t hash[32])
{
    uint8_t shared_bytes[HFOC_SSH_CRYPTO_SHARED_MAX];
    mbedtls_sha256_context context;
    const uint8_t *q_s;
    size_t shared_length;
    bool ok = false;

    mbedtls_sha256_init(&context);
    if (!crypto->shared_ready || !crypto->ephemeral_ready) {
        goto done;
    }
    q_s = hfoc_ssh_crypto_kex_public(crypto);
    if (q_s == NULL) {
        goto done;
    }
    shared_length = mbedtls_mpi_size(&crypto->shared);
    if (shared_length > sizeof(shared_bytes) ||
        mbedtls_mpi_write_binary(&crypto->shared, shared_bytes,
                                 shared_length) != 0) {
        goto done;
    }
    ok = mbedtls_sha256_starts(&context, 0) == 0 &&
         ssh_hash_string(&context, v_c, v_c_length) &&
         ssh_hash_string(&context, v_s, v_s_length) &&
         ssh_hash_string(&context, i_c, i_c_length) &&
         ssh_hash_string(&context, i_s, i_s_length) &&
         ssh_hash_string(&context, crypto->host_blob,
                         crypto->host_blob_length) &&
         ssh_hash_string(&context, q_c, q_c_length) &&
         ssh_hash_string(&context, q_s, 65U) &&
         ssh_hash_mpint(&context, shared_bytes, shared_length) &&
         mbedtls_sha256_finish(&context, hash) == 0;
done:
    mbedtls_sha256_free(&context);
    ssh_secret_zero(shared_bytes, sizeof(shared_bytes));
    return ok;
}

static bool ssh_signature_encode(mbedtls_mpi *r, mbedtls_mpi *s,
                                 uint8_t *signature, size_t *signature_length)
{
    uint8_t inner[HFOC_SSH_CRYPTO_SIGNATURE_MAX];
    struct hfoc_ssh_writer inner_writer;
    struct hfoc_ssh_writer outer_writer;
    size_t r_size = mbedtls_mpi_size(r);
    size_t s_size = mbedtls_mpi_size(s);
    uint8_t r_bytes[33];
    uint8_t s_bytes[33];
    bool ok = false;

    if (r_size > 32U || s_size > 32U ||
        mbedtls_mpi_write_binary(r, r_bytes, r_size) != 0 ||
        mbedtls_mpi_write_binary(s, s_bytes, s_size) != 0) {
        goto done;
    }
    hfoc_ssh_writer_init(&inner_writer, inner, sizeof(inner));
    (void)hfoc_ssh_write_mpint(&inner_writer, r_bytes, r_size);
    (void)hfoc_ssh_write_mpint(&inner_writer, s_bytes, s_size);
    if (inner_writer.failed) {
        goto done;
    }
    hfoc_ssh_writer_init(&outer_writer, signature, *signature_length);
    (void)hfoc_ssh_write_cstring(&outer_writer, ssh_host_algorithm);
    (void)hfoc_ssh_write_string(&outer_writer, inner, inner_writer.length);
    if (outer_writer.failed) {
        goto done;
    }
    *signature_length = outer_writer.length;
    ok = true;
done:
    ssh_secret_zero(inner, sizeof(inner));
    ssh_secret_zero(r_bytes, sizeof(r_bytes));
    ssh_secret_zero(s_bytes, sizeof(s_bytes));
    return ok;
}

bool hfoc_ssh_crypto_sign(struct hfoc_ssh_crypto *crypto, const uint8_t hash[32],
                     hfoc_ssh_rng_fn rng, void *rng_context, uint8_t *signature,
                     size_t *signature_length)
{
    mbedtls_mpi r;
    mbedtls_mpi s;
    uint8_t digest[32];
    bool ok;

    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    /* OpenSSH's sshkey_sign hashes the supplied message with the key's hash
     * algorithm; for the host key the supplied message is H, so the ECDSA
     * digest is SHA-256(H), not H itself. */
    ok = mbedtls_sha256(hash, 32U, digest, 0) == 0 &&
         mbedtls_ecdsa_sign(&crypto->host.MBEDTLS_PRIVATE(grp), &r, &s,
                            &crypto->host.MBEDTLS_PRIVATE(d), digest, 32U,
                            rng, rng_context) == 0 &&
         ssh_signature_encode(&r, &s, signature, signature_length);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    ssh_secret_zero(&s, sizeof(s));
    ssh_secret_zero(&r, sizeof(r));
    ssh_secret_zero(digest, sizeof(digest));
    return ok;
}

bool hfoc_ssh_crypto_verify_client(const uint8_t *key_blob, size_t key_blob_length,
                              const uint8_t hash[32],
                              const uint8_t *signature,
                              size_t signature_length)
{
    mbedtls_ecp_group group;
    mbedtls_ecp_point public_key;
    mbedtls_mpi r;
    mbedtls_mpi s;
    struct hfoc_ssh_reader key_reader;
    struct hfoc_ssh_reader signature_reader;
    struct hfoc_ssh_reader inner_reader;
    const uint8_t *field;
    const uint8_t *inner;
    uint32_t field_length;
    uint32_t inner_length;
    bool ok = false;

    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&public_key);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) != 0) {
        goto done;
    }
    hfoc_ssh_reader_init(&key_reader, key_blob, key_blob_length);
    if (!hfoc_ssh_read_string(&key_reader, &field, &field_length) ||
        field_length != strlen(ssh_host_algorithm) ||
        memcmp(field, ssh_host_algorithm, field_length) != 0 ||
        !hfoc_ssh_read_string(&key_reader, &field, &field_length) ||
        field_length != strlen(ssh_curve_name) ||
        memcmp(field, ssh_curve_name, field_length) != 0 ||
        !hfoc_ssh_read_string(&key_reader, &field, &field_length) ||
        field_length != 65U ||
        mbedtls_ecp_point_read_binary(&group, &public_key, field,
                                      field_length) != 0) {
        goto done;
    }
    hfoc_ssh_reader_init(&signature_reader, signature, signature_length);
    if (!hfoc_ssh_read_string(&signature_reader, &field, &field_length) ||
        field_length != strlen(ssh_host_algorithm) ||
        memcmp(field, ssh_host_algorithm, field_length) != 0 ||
        !hfoc_ssh_read_string(&signature_reader, &inner, &inner_length)) {
        goto done;
    }
    hfoc_ssh_reader_init(&inner_reader, inner, inner_length);
    if (!hfoc_ssh_read_mpint(&inner_reader, &field, &field_length) ||
        mbedtls_mpi_read_binary(&r, field, field_length) != 0 ||
        !hfoc_ssh_read_mpint(&inner_reader, &field, &field_length) ||
        mbedtls_mpi_read_binary(&s, field, field_length) != 0 ||
        !hfoc_ssh_reader_done(&inner_reader)) {
        goto done;
    }
    ok = mbedtls_ecdsa_verify(&group, hash, 32U, &public_key, &r, &s) == 0;
done:
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&public_key);
    mbedtls_ecp_group_free(&group);
    return ok;
}

bool hfoc_ssh_crypto_kdf(struct hfoc_ssh_crypto *crypto, const uint8_t hash[32],
                    const uint8_t session_id[32], uint8_t letter,
                    uint8_t *output, size_t output_length)
{
    uint8_t buffer[256];
    uint8_t digest[32];
    uint8_t shared_bytes[HFOC_SSH_CRYPTO_SHARED_MAX];
    size_t shared_length;
    struct hfoc_ssh_writer writer;
    bool ok = false;

    if (!crypto->shared_ready || output_length > sizeof(digest)) {
        return false;
    }
    shared_length = mbedtls_mpi_size(&crypto->shared);
    if (shared_length > sizeof(shared_bytes) ||
        mbedtls_mpi_write_binary(&crypto->shared, shared_bytes,
                                 shared_length) != 0) {
        goto done;
    }
    hfoc_ssh_writer_init(&writer, buffer, sizeof(buffer));
    (void)hfoc_ssh_write_mpint(&writer, shared_bytes, shared_length);
    (void)hfoc_ssh_write_bytes(&writer, hash, 32U);
    (void)hfoc_ssh_write_u8(&writer, letter);
    (void)hfoc_ssh_write_bytes(&writer, session_id, 32U);
    if (writer.failed ||
        mbedtls_sha256(buffer, writer.length, digest, 0) != 0) {
        goto done;
    }
    memcpy(output, digest, output_length);
    ok = true;
done:
    ssh_secret_zero(buffer, sizeof(buffer));
    ssh_secret_zero(digest, sizeof(digest));
    ssh_secret_zero(shared_bytes, sizeof(shared_bytes));
    return ok;
}

static void increment_iv(uint8_t iv[12])
{
    int index;

    for (index = 11; index >= 4; --index) {
        if (++iv[index] != 0U) {
            break;
        }
    }
}

bool hfoc_ssh_crypto_aead_init(struct hfoc_ssh_crypto *crypto, bool tx,
                          const uint8_t *key, size_t key_length,
                          const uint8_t iv[12])
{
    mbedtls_gcm_context *gcm = tx ? &crypto->tx : &crypto->rx;

    if (mbedtls_gcm_setkey(gcm, MBEDTLS_CIPHER_ID_AES, key,
                           (unsigned int)(key_length * 8U)) != 0) {
        return false;
    }
    memcpy(tx ? crypto->tx_iv : crypto->rx_iv, iv, 12U);
    if (tx) {
        crypto->tx_active = true;
    } else {
        crypto->rx_active = true;
    }
    return true;
}

bool hfoc_ssh_crypto_seal(struct hfoc_ssh_crypto *crypto, const uint8_t aad[4],
                     const uint8_t *plain, size_t length, uint8_t *cipher,
                     uint8_t tag[16])
{
    if (!crypto->tx_active ||
        mbedtls_gcm_crypt_and_tag(&crypto->tx, MBEDTLS_GCM_ENCRYPT, length,
                                  crypto->tx_iv, 12U, aad, 4U, plain, cipher,
                                  16U, tag) != 0) {
        return false;
    }
    increment_iv(crypto->tx_iv);
    return true;
}

bool hfoc_ssh_crypto_open(struct hfoc_ssh_crypto *crypto, const uint8_t aad[4],
                     const uint8_t *cipher, size_t length, const uint8_t tag[16],
                     uint8_t *plain)
{
    if (!crypto->rx_active) {
        return false;
    }
    if (mbedtls_gcm_auth_decrypt(&crypto->rx, length, crypto->rx_iv, 12U,
                                 aad, 4U, tag, 16U, cipher, plain) != 0) {
        return false;
    }
    increment_iv(crypto->rx_iv);
    return true;
}

/* ------------------------------------------------------------------ identity */

static bool hfoc_ssh_crypto_build_host_blob(
    const mbedtls_ecp_group *group, const mbedtls_ecp_point *public_point,
    uint8_t blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX], size_t *blob_length,
    uint8_t fingerprint[32])
{
    struct hfoc_ssh_writer writer;
    uint8_t point[65];
    size_t point_length = 0U;

    if (mbedtls_ecp_point_write_binary(
            group, public_point, MBEDTLS_ECP_PF_UNCOMPRESSED,
            &point_length, point, sizeof(point)) != 0 ||
        point_length != sizeof(point)) {
        return false;
    }
    hfoc_ssh_writer_init(&writer, blob, HFOC_SSH_CRYPTO_HOST_BLOB_MAX);
    (void)hfoc_ssh_write_cstring(&writer, ssh_host_algorithm);
    (void)hfoc_ssh_write_cstring(&writer, ssh_curve_name);
    (void)hfoc_ssh_write_string(&writer, point, point_length);
    if (writer.failed) {
        return false;
    }
    *blob_length = writer.length;
    return mbedtls_sha256(blob, writer.length, fingerprint, 0) == 0;
}

bool hfoc_ssh_crypto_host_key_create(
    uint8_t scalar[32], uint8_t blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX],
    size_t *blob_length, uint8_t fingerprint[32], hfoc_ssh_rng_fn rng,
    void *rng_context)
{
    mbedtls_ecdsa_context key;
    bool ok;

    if (scalar == NULL || blob == NULL || blob_length == NULL ||
        fingerprint == NULL || rng == NULL) {
        return false;
    }
    mbedtls_ecdsa_init(&key);
    ok = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, &key, rng,
                             rng_context) == 0 &&
         mbedtls_mpi_write_binary(&key.MBEDTLS_PRIVATE(d), scalar, 32U) == 0 &&
         hfoc_ssh_crypto_build_host_blob(
             &key.MBEDTLS_PRIVATE(grp), &key.MBEDTLS_PRIVATE(Q), blob,
             blob_length, fingerprint);
    mbedtls_ecdsa_free(&key);
    ssh_secret_zero(&key, sizeof(key));
    return ok;
}

bool hfoc_ssh_crypto_host_key_derive(
    const uint8_t scalar[32],
    uint8_t blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX], size_t *blob_length,
    uint8_t fingerprint[32], hfoc_ssh_rng_fn rng, void *rng_context)
{
    mbedtls_ecdsa_context key;
    bool ok;

    if (scalar == NULL || blob == NULL || blob_length == NULL ||
        fingerprint == NULL || rng == NULL) {
        return false;
    }
    mbedtls_ecdsa_init(&key);
    ok = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &key, scalar,
                              32U) == 0 &&
         mbedtls_ecp_mul(&key.MBEDTLS_PRIVATE(grp), &key.MBEDTLS_PRIVATE(Q),
                         &key.MBEDTLS_PRIVATE(d),
                         &key.MBEDTLS_PRIVATE(grp).G, rng,
                         rng_context) == 0 &&
         hfoc_ssh_crypto_build_host_blob(
             &key.MBEDTLS_PRIVATE(grp), &key.MBEDTLS_PRIVATE(Q), blob,
             blob_length, fingerprint);
    mbedtls_ecdsa_free(&key);
    ssh_secret_zero(&key, sizeof(key));
    return ok;
}

bool hfoc_ssh_crypto_public_blob_validate(const uint8_t *blob, size_t length)
{
    mbedtls_ecp_group group;
    mbedtls_ecp_point point;
    struct hfoc_ssh_reader reader;
    const uint8_t *field;
    uint32_t field_length;
    bool ok = false;

    if (blob == NULL || length == 0U) {
        return false;
    }
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&point);
    if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) != 0) {
        goto done;
    }
    hfoc_ssh_reader_init(&reader, blob, length);
    if (!hfoc_ssh_read_string(&reader, &field, &field_length) ||
        field_length != strlen(ssh_host_algorithm) ||
        memcmp(field, ssh_host_algorithm, field_length) != 0 ||
        !hfoc_ssh_read_string(&reader, &field, &field_length) ||
        field_length != strlen(ssh_curve_name) ||
        memcmp(field, ssh_curve_name, field_length) != 0 ||
        !hfoc_ssh_read_string(&reader, &field, &field_length) ||
        field_length != 65U ||
        mbedtls_ecp_point_read_binary(&group, &point, field,
                                      field_length) != 0 ||
        !hfoc_ssh_reader_done(&reader)) {
        goto done;
    }
    ok = true;
done:
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&group);
    return ok;
}

bool hfoc_ssh_crypto_public_blob_fingerprint(const uint8_t *blob, size_t length,
                                             uint8_t fingerprint[32])
{
    if (blob == NULL || fingerprint == NULL || length == 0U) {
        return false;
    }
    return mbedtls_sha256(blob, length, fingerprint, 0) == 0;
}

#else
typedef int hfoc_ssh_crypto_translation_unit_guard;
#endif
