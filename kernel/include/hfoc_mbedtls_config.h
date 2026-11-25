#ifndef HFOC_MBEDTLS_CONFIG_H
#define HFOC_MBEDTLS_CONFIG_H

#include "hfoc_mbedtls_alloc.h"

/*
 * HFOC OS mbedTLS configuration (item 262; profile version 5, item 508).
 *
 * This is the only mbedTLS configuration header used by HFOC. The profile is
 * frozen as TLS 1.3 with SNI-based certificate selection, both the server and
 * the client role, TLS_AES_128_GCM_SHA256, P-256 ECDHE/ECDSA, DER X.509
 * parsing and writing, and CTR-DRBG.  Every disabled family is explicit so a
 * future configuration change is a reviewed change.  SNI chooses a certificate
 * presentation for the handshake; it is never trust, authentication, or an authorization decision,
 * and it changes nothing after the handshake (item 507).
 *
 * The client role exists for one caller: the ACME module obtains the
 * appliance's own certificate from the declared ACME endpoint (item 508).
 * The appliance never becomes a general TLS client: PEM stays disabled (the
 * trust anchor in the image is DER), no filesystem, socket or timing layer is
 * compiled, and the verifier keeps every other family forbidden verbatim.
 *
 * ITEM-508 increment 33, owner decision of 2026-10-06: RSA verification is
 * enabled **for the CA chain only**.  The image's one trust anchor is ISRG
 * Root X1 (RSA-4096) and the Let's Encrypt intermediates R10/R11 are RSA, so
 * without RSA the module could not parse its own anchor and answered
 * `ANCHOR_UNAVAILABLE` instead of verifying.  `MBEDTLS_RSA_C` and
 * `MBEDTLS_PKCS1_V15` are therefore defined: a *verification* capability the
 * X.509 path uses through `pk`/`x509`.  Nothing else becomes RSA: the ACME
 * account key stays ES256 (P-256 + SHA-256), the appliance's own certificate
 * stays P-256, no RSA key is generated, stored or signs anything, PKCS#1 v2.1
 * (PSS/OAEP) stays disabled, and our sources never call `mbedtls_rsa_init`.
 */

#define HFOC_MBEDTLS_CONFIG_VERSION 5
#define MBEDTLS_CONFIG_VERSION 0x03060700

/* All mbedTLS allocations route through fixed HFOC pools (item 263). */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_CALLOC_MACRO hfoc_mbedtls_calloc
#define MBEDTLS_PLATFORM_FREE_MACRO hfoc_mbedtls_free
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS

/* TLS 1.3 server and client; no PSA crypto implementation is compiled. */
#define MBEDTLS_PSA_CRYPTO_CLIENT
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
/*
 * Item 508: the client role the ACME module needs.  It is not a general
 * client: the only outbound connection is the declared ACME endpoint, the
 * trust anchor is named in the image, the hostname is verified and any
 * verification flag refuses the order.  Enabling this define alone adds the
 * client half of the TLS 1.3 state machine and its X.509 verification; every
 * protocol, platform and algorithm family the profile still rejects below
 * (TLS 1.2, DTLS, renegotiation, tickets, cache, cookie, ALPN, PKCS#1 v2.1,
 * PEM, filesystem, sockets, timing, threading) stays rejected verbatim.
 * ITEM-508 increment 33 adds RSA *verification* for the CA chain, under the
 * owner's 2026-10-06 decision; the reason and the boundary are in the header
 * comment above.
 */
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION

/* Cipher and hash set: AES-128-GCM-SHA256 only. */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_HKDF_C

/* P-256 ECDHE/ECDSA keys and DER X.509 certificate parsing. */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
/*
 * Item 508 increment 22: the write side of X.509, and only the write side.
 * The profile has carried these families since the first-boot self-signed
 * certificate of item 386 and the ACME finalize of item 508 needs the CSR
 * writer; the profile now owns them in one place instead of the serving delta
 * redefining them.  They are write-side only: the two callers are the
 * certificate owner (item 386) and the ACME account module, no request path
 * reaches them, no CSR or CRL is ever parsed, and PEM/Base64 stay rejected
 * below.  Every one of these is a positive contract in
 * `scripts/verify-mbedtls-config.sh` (version 4).
 */
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_X509_CSR_WRITE_C
#define MBEDTLS_PK_WRITE_C

/* CTR-DRBG without the mbedTLS entropy module; item 264 supplies the source. */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* Explicitly reject feature families that must never be enabled. */
#undef MBEDTLS_SSL_PROTO_TLS1_2
#undef MBEDTLS_SSL_PROTO_TLS1_1
#undef MBEDTLS_SSL_PROTO_TLS1
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_RENEGOTIATION
#undef MBEDTLS_SSL_SESSION_TICKETS
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_ALPN
#undef MBEDTLS_SSL_KEYING_MATERIAL_EXPORT
#undef MBEDTLS_FS_IO
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_THREADING_C
#undef MBEDTLS_DEBUG_C
/*
 * ITEM-508 increment 33: RSA verification only.  `MBEDTLS_RSA_C` and
 * `MBEDTLS_PKCS1_V15` let the X.509 path verify the RSA signatures of the
 * CA chain (ISRG Root X1, R10/R11) through `pk`; PKCS#1 v2.1, key generation
 * and every RSA key this appliance owns stay off (see the header comment).
 */
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#undef MBEDTLS_PKCS1_V21
#undef MBEDTLS_MD5_C
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_SHA1_C
#undef MBEDTLS_SHA224_C
#undef MBEDTLS_SHA384_C
#undef MBEDTLS_SHA512_C
#undef MBEDTLS_SHA3_C
#undef MBEDTLS_CCM_C
#undef MBEDTLS_CHACHA20_C
#undef MBEDTLS_CHACHAPOLY_C
#undef MBEDTLS_POLY1305_C
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_PEM_PARSE_C
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_BASE64_C
#undef MBEDTLS_X509_CRL_PARSE_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_PSA_CRYPTO_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_CRYPTO_ITS_FILE_C
#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP384R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP521R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED
#undef MBEDTLS_ECP_DP_CURVE25519_ENABLED
#undef MBEDTLS_ECP_DP_CURVE448_ENABLED

#endif
