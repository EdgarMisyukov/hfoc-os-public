#ifndef HFOC_SSH_HOST_CONFIG_H
#define HFOC_SSH_HOST_CONFIG_H

/*
 * ITEM-601 host profile for the SSHv2 wire/crypto/transport proofs.
 *
 * The production image compiles mbedTLS with hfoc_mbedtls_tls_config.h; this
 * minimal profile enables exactly the primitives the SSH modules use, so the
 * host proof links a small closure.  No production file includes this header.
 */
#define MBEDTLS_HAVE_INT32
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C

#endif
