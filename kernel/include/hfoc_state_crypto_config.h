#ifndef HFOC_STATE_CRYPTO_CONFIG_H
#define HFOC_STATE_CRYPTO_CONFIG_H

/*
 * HFOC OS state-encryption crypto profile (ITEM-373).
 *
 * This is deliberately not the TLS profile and not the signature-verification
 * profile: it links exactly one Mbed TLS primitive, AES-256 in counter mode,
 * for the sealed state volume.  HMAC-SHA-256 and PBKDF2-HMAC-SHA-256 are built
 * in appliance_state_crypto.c on top of the HFOC SHA-256 implementation, so
 * this profile pulls in no message-digest, cipher, TLS, X.509, PK, PSA or RNG
 * family.
 *
 * MBEDTLS_PLATFORM_ZEROIZE_ALT is declared and the symbol is provided by the
 * embedding image: the production kernel takes it from the ITEM-372
 * verification profile, and the focused host proof defines it in the test.
 * This keeps exactly one definition in every image.
 */

#define MBEDTLS_CONFIG_VERSION 0x03060700

#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_MODE_CTR

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_ZEROIZE_ALT

/* Explicitly reject every other Mbed TLS family. */
#undef MBEDTLS_PLATFORM_MEMORY
#undef MBEDTLS_GCM_C
#undef MBEDTLS_CCM_C
#undef MBEDTLS_CHACHA20_C
#undef MBEDTLS_CHACHAPOLY_C
#undef MBEDTLS_POLY1305_C
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_CIPHER_C
#undef MBEDTLS_CIPHER_MODE_CBC
#undef MBEDTLS_CIPHER_MODE_CFB
#undef MBEDTLS_CIPHER_MODE_OFB
#undef MBEDTLS_CIPHER_MODE_XTS
#undef MBEDTLS_MD_C
#undef MBEDTLS_SHA1_C
#undef MBEDTLS_SHA224_C
#undef MBEDTLS_SHA256_C
#undef MBEDTLS_SHA384_C
#undef MBEDTLS_SHA512_C
#undef MBEDTLS_SHA3_C
#undef MBEDTLS_HKDF_C
#undef MBEDTLS_CTR_DRBG_C
#undef MBEDTLS_ENTROPY_C
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_SSL_TLS_C
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_CLI_C
#undef MBEDTLS_X509_CRT_PARSE_C
#undef MBEDTLS_PK_C
#undef MBEDTLS_PK_PARSE_C
#undef MBEDTLS_PSA_CRYPTO_C
#undef MBEDTLS_BIGNUM_C
#undef MBEDTLS_ECP_C
#undef MBEDTLS_ECDSA_C
#undef MBEDTLS_ECDH_C
#undef MBEDTLS_ASN1_PARSE_C
#undef MBEDTLS_ASN1_WRITE_C
#undef MBEDTLS_OID_C

#endif
