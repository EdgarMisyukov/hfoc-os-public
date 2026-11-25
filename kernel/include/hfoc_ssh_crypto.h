#ifndef HFOC_SSH_CRYPTO_H
#define HFOC_SSH_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"

#define HFOC_SSH_CRYPTO_HOST_BLOB_MAX 160U
#define HFOC_SSH_CRYPTO_SHARED_MAX 40U
#define HFOC_SSH_CRYPTO_SIGNATURE_MAX 160U

typedef int (*hfoc_ssh_rng_fn)(void *context, unsigned char *output, size_t length);

struct hfoc_ssh_crypto {
    mbedtls_ecp_group group;
    mbedtls_ecdsa_context host;
    bool host_loaded;
    mbedtls_mpi ephemeral_secret;
    mbedtls_ecp_point ephemeral_public;
    bool ephemeral_ready;
    mbedtls_mpi shared;
    bool shared_ready;
    uint8_t host_blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX];
    size_t host_blob_length;
    uint8_t host_fingerprint[32];
    mbedtls_gcm_context tx;
    mbedtls_gcm_context rx;
    bool tx_active;
    bool rx_active;
    uint8_t tx_iv[12];
    uint8_t rx_iv[12];
};

bool hfoc_ssh_crypto_init(struct hfoc_ssh_crypto *crypto);
void hfoc_ssh_crypto_free(struct hfoc_ssh_crypto *crypto);

bool hfoc_ssh_crypto_host_key_load(struct hfoc_ssh_crypto *crypto,
                              const uint8_t scalar[32], hfoc_ssh_rng_fn rng,
                              void *rng_context);
bool hfoc_ssh_crypto_host_key_generate(struct hfoc_ssh_crypto *crypto, hfoc_ssh_rng_fn rng,
                                  void *rng_context);
/*
 * ITEM-601 identity helpers: the durable owner needs exactly the scalar, the
 * public blob and its SHA-256 fingerprint, without holding a session context.
 */
bool hfoc_ssh_crypto_host_key_create(
    uint8_t scalar[32], uint8_t blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX],
    size_t *blob_length, uint8_t fingerprint[32], hfoc_ssh_rng_fn rng,
    void *rng_context);
bool hfoc_ssh_crypto_host_key_derive(
    const uint8_t scalar[32],
    uint8_t blob[HFOC_SSH_CRYPTO_HOST_BLOB_MAX], size_t *blob_length,
    uint8_t fingerprint[32], hfoc_ssh_rng_fn rng, void *rng_context);
bool hfoc_ssh_crypto_public_blob_validate(const uint8_t *blob, size_t length);
bool hfoc_ssh_crypto_public_blob_fingerprint(const uint8_t *blob, size_t length,
                                             uint8_t fingerprint[32]);
const uint8_t *hfoc_ssh_crypto_host_blob(const struct hfoc_ssh_crypto *crypto,
                                    size_t *length);
const uint8_t *hfoc_ssh_crypto_host_fingerprint(const struct hfoc_ssh_crypto *crypto);

bool hfoc_ssh_crypto_kex_start(struct hfoc_ssh_crypto *crypto, hfoc_ssh_rng_fn rng,
                          void *rng_context);
const uint8_t *hfoc_ssh_crypto_kex_public(const struct hfoc_ssh_crypto *crypto);
bool hfoc_ssh_crypto_kex_shared(struct hfoc_ssh_crypto *crypto, const uint8_t *peer,
                           size_t peer_length, hfoc_ssh_rng_fn rng,
                           void *rng_context, uint8_t *mpint,
                           size_t *mpint_length);

bool hfoc_ssh_crypto_exchange_hash(
    struct hfoc_ssh_crypto *crypto, const uint8_t *v_c, size_t v_c_length,
    const uint8_t *v_s, size_t v_s_length, const uint8_t *i_c,
    size_t i_c_length, const uint8_t *i_s, size_t i_s_length,
    const uint8_t *q_c, size_t q_c_length, uint8_t hash[32]);
bool hfoc_ssh_crypto_sign(struct hfoc_ssh_crypto *crypto, const uint8_t hash[32],
                     hfoc_ssh_rng_fn rng, void *rng_context, uint8_t *signature,
                     size_t *signature_length);
bool hfoc_ssh_crypto_verify_client(const uint8_t *key_blob, size_t key_blob_length,
                              const uint8_t hash[32],
                              const uint8_t *signature,
                              size_t signature_length);
bool hfoc_ssh_crypto_kdf(struct hfoc_ssh_crypto *crypto, const uint8_t hash[32],
                    const uint8_t session_id[32], uint8_t letter,
                    uint8_t *output, size_t output_length);
bool hfoc_ssh_crypto_aead_init(struct hfoc_ssh_crypto *crypto, bool tx,
                          const uint8_t *key, size_t key_length,
                          const uint8_t iv[12]);
bool hfoc_ssh_crypto_seal(struct hfoc_ssh_crypto *crypto, const uint8_t aad[4],
                     const uint8_t *plain, size_t length, uint8_t *cipher,
                     uint8_t tag[16]);
bool hfoc_ssh_crypto_open(struct hfoc_ssh_crypto *crypto, const uint8_t aad[4],
                     const uint8_t *cipher, size_t length, const uint8_t tag[16],
                     uint8_t *plain);

#endif
