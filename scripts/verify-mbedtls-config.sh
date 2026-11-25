#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
CONFIG=${1:-"$ROOT/kernel/include/hfoc_mbedtls_config.h"}
MBEDTLS_INCLUDE="$ROOT/third_party/mbedtls/include"
KERNEL_INCLUDE="$ROOT/kernel/include"

shift || true
SYMBOL_SOURCES=()
EXCEPT_SYMBOLS=()
EXCEPT_SOURCES=()

fail() {
    echo "mbedTLS configuration verification failed: $*" >&2
    exit 1
}

# ITEM-508 increment 12: a forbidden symbol may be excepted for exactly one
# named source with `--except=<symbol>:<source>`.  The exception is checked
# both ways below - it must name a symbol this verifier forbids and a source
# this invocation checks - so a mistyped exception is a refusal and never a
# silent no-op.
for argument in "$@"; do
    case $argument in
    --except=*)
        exception=${argument#--except=}
        [[ $exception == *:* ]] ||
            fail "malformed exception (expected symbol:source): $argument"
        EXCEPT_SYMBOLS+=("${exception%%:*}")
        EXCEPT_SOURCES+=("${exception#*:}")
        ;;
    *)
        SYMBOL_SOURCES+=("$argument")
        ;;
    esac
done

[[ -f "$CONFIG" ]] || fail "missing config: $CONFIG"
[[ -f "$MBEDTLS_INCLUDE/mbedtls/build_info.h" ]] || {
    fail "missing vendored mbedTLS headers"
}

CONFIG_DIR=$(cd -- "$(dirname -- "$CONFIG")" && pwd -P)
CONFIG_BASE=$(basename -- "$CONFIG")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [[ "$CONFIG_BASE" != "hfoc_mbedtls_config.h" ]]; then
    cp "$CONFIG" "$WORK/hfoc_mbedtls_config.h"
    CONFIG_DIR="$WORK"
fi

cat >"$WORK/probe.c" <<'EOF'
#include "mbedtls/build_info.h"
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"

#ifndef HFOC_MBEDTLS_CONFIG_VERSION
#error HFOC_MBEDTLS_CONFIG_VERSION is required
#endif
#if HFOC_MBEDTLS_CONFIG_VERSION != 5
#error HFOC_MBEDTLS_CONFIG_VERSION must be 5
#endif
#if MBEDTLS_VERSION_MAJOR != 3 || MBEDTLS_VERSION_MINOR != 6 || \
    MBEDTLS_VERSION_PATCH != 7
#error mbedTLS version must be 3.6.7
#endif

/* Required HFOC allocator mapping. */
#if !defined(MBEDTLS_PLATFORM_MEMORY)
#error MBEDTLS_PLATFORM_MEMORY is required
#endif
#if !defined(MBEDTLS_PLATFORM_CALLOC_MACRO)
#error MBEDTLS_PLATFORM_CALLOC_MACRO is required
#endif
#if !defined(MBEDTLS_PLATFORM_FREE_MACRO)
#error MBEDTLS_PLATFORM_FREE_MACRO is required
#endif
#if !defined(MBEDTLS_PLATFORM_NO_STD_FUNCTIONS)
#error MBEDTLS_PLATFORM_NO_STD_FUNCTIONS is required
#endif
#if defined(MBEDTLS_PLATFORM_STD_CALLOC)
#error MBEDTLS_PLATFORM_STD_CALLOC is forbidden with the HFOC allocator
#endif
#if defined(MBEDTLS_PLATFORM_STD_FREE)
#error MBEDTLS_PLATFORM_STD_FREE is forbidden with the HFOC allocator
#endif
#ifndef HFOC_MBEDTLS_ALLOC_VERSION
#error HFOC_MBEDTLS_ALLOC_VERSION is required
#endif
#if HFOC_MBEDTLS_ALLOC_VERSION != 1
#error HFOC_MBEDTLS_ALLOC_VERSION must be 1
#endif

static void *hfoc_mbedtls_allocator_probe(size_t allocation_size)
{
    void *pointer;

    pointer = mbedtls_calloc(1U, allocation_size);
    mbedtls_free(pointer);
    return 0;
}

/* Required TLS 1.3 server and client profile. */
#if !defined(MBEDTLS_PSA_CRYPTO_CLIENT)
#error MBEDTLS_PSA_CRYPTO_CLIENT is required
#endif
#if !defined(MBEDTLS_SSL_TLS_C)
#error MBEDTLS_SSL_TLS_C is required
#endif
#if !defined(MBEDTLS_SSL_SRV_C)
#error MBEDTLS_SSL_SRV_C is required
#endif
/*
 * Item 508: the client role exists for the ACME module alone.  The profile has
 * to carry it, and the per-source symbol check below keeps every other
 * translation unit out of the client path.
 */
#if !defined(MBEDTLS_SSL_CLI_C)
#error MBEDTLS_SSL_CLI_C is required for the ACME client role
#endif
#if !defined(MBEDTLS_SSL_PROTO_TLS1_3)
#error MBEDTLS_SSL_PROTO_TLS1_3 is required
#endif
#if !defined(MBEDTLS_SSL_KEEP_PEER_CERTIFICATE)
#error MBEDTLS_SSL_KEEP_PEER_CERTIFICATE is required
#endif
#if !defined(MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED)
#error MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED is required
#endif
/* SNI selects the certificate presentation for the handshake by name (item
 * 507); it is never trust and never authentication.  The name-bound
 * certificate store and the selection callback live in the serving
 * composition, not in this profile. */
#ifndef MBEDTLS_SSL_SERVER_NAME_INDICATION
#error MBEDTLS_SSL_SERVER_NAME_INDICATION is required
#endif
#if defined(MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED)
#error TLS 1.3 PSK key exchange is forbidden
#endif
#if defined(MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED)
#error TLS 1.3 PSK-EPHEMERAL key exchange is forbidden
#endif

/* Required frozen crypto, X.509, and DRBG set. */
#if !defined(MBEDTLS_CIPHER_C)
#error MBEDTLS_CIPHER_C is required
#endif
#if !defined(MBEDTLS_AES_C)
#error MBEDTLS_AES_C is required
#endif
#if !defined(MBEDTLS_GCM_C)
#error MBEDTLS_GCM_C is required
#endif
#if !defined(MBEDTLS_MD_C)
#error MBEDTLS_MD_C is required
#endif
#if !defined(MBEDTLS_SHA256_C)
#error MBEDTLS_SHA256_C is required
#endif
#if !defined(MBEDTLS_MD_CAN_SHA256)
#error MBEDTLS_MD_CAN_SHA256 is required
#endif
#if defined(MBEDTLS_MD_CAN_SHA384) || defined(MBEDTLS_MD_CAN_SHA512) || \
    defined(MBEDTLS_MD_CAN_SHA1) || defined(MBEDTLS_MD_CAN_MD5) || \
    defined(MBEDTLS_MD_CAN_RIPEMD160) || defined(MBEDTLS_MD_CAN_SHA3_256)
#error only SHA-256 is allowed in the mbedTLS hash set
#endif
#if !defined(MBEDTLS_HKDF_C)
#error MBEDTLS_HKDF_C is required
#endif
#if !defined(MBEDTLS_BIGNUM_C)
#error MBEDTLS_BIGNUM_C is required
#endif
#if !defined(MBEDTLS_ECP_C)
#error MBEDTLS_ECP_C is required
#endif
#if !defined(MBEDTLS_ECP_DP_SECP256R1_ENABLED)
#error MBEDTLS_ECP_DP_SECP256R1_ENABLED is required
#endif
#if defined(MBEDTLS_ECP_DP_SECP192R1_ENABLED) || \
    defined(MBEDTLS_ECP_DP_SECP224R1_ENABLED) || \
    defined(MBEDTLS_ECP_DP_SECP384R1_ENABLED) || \
    defined(MBEDTLS_ECP_DP_SECP521R1_ENABLED) || \
    defined(MBEDTLS_ECP_DP_SECP192K1_ENABLED) || \
    defined(MBEDTLS_ECP_DP_SECP224K1_ENABLED) || \
    defined(MBEDTLS_ECP_DP_SECP256K1_ENABLED) || \
    defined(MBEDTLS_ECP_DP_BP256R1_ENABLED) || \
    defined(MBEDTLS_ECP_DP_BP384R1_ENABLED) || \
    defined(MBEDTLS_ECP_DP_BP512R1_ENABLED) || \
    defined(MBEDTLS_ECP_DP_CURVE25519_ENABLED) || \
    defined(MBEDTLS_ECP_DP_CURVE448_ENABLED)
#error only secp256r1 is allowed in the mbedTLS group set
#endif
#if !defined(MBEDTLS_ECDH_C)
#error MBEDTLS_ECDH_C is required
#endif
#if !defined(MBEDTLS_ECDSA_C)
#error MBEDTLS_ECDSA_C is required
#endif
#if !defined(MBEDTLS_PK_CAN_ECDSA_SIGN)
#error MBEDTLS_PK_CAN_ECDSA_SIGN is required
#endif
#if !defined(MBEDTLS_ASN1_PARSE_C) || !defined(MBEDTLS_ASN1_WRITE_C) || \
    !defined(MBEDTLS_OID_C)
#error ASN.1/OID prerequisites are required
#endif
#if !defined(MBEDTLS_PK_C) || !defined(MBEDTLS_PK_PARSE_C)
#error PK parse prerequisites are required
#endif
#if !defined(MBEDTLS_X509_USE_C) || !defined(MBEDTLS_X509_CRT_PARSE_C)
#error X.509 parse prerequisites are required
#endif
#if !defined(MBEDTLS_CTR_DRBG_C)
#error MBEDTLS_CTR_DRBG_C is required
#endif
#if !defined(MBEDTLS_NO_PLATFORM_ENTROPY)
#error MBEDTLS_NO_PLATFORM_ENTROPY is required
#endif

/* Forbidden protocol, platform, and algorithm families. */
#if defined(MBEDTLS_SSL_PROTO_TLS1_2)
#error TLS 1.2 is forbidden
#endif
#if defined(MBEDTLS_SSL_PROTO_TLS1_1) || defined(MBEDTLS_SSL_PROTO_TLS1)
#error legacy TLS is forbidden
#endif
#if defined(MBEDTLS_SSL_PROTO_DTLS)
#error DTLS is forbidden
#endif
#if defined(MBEDTLS_SSL_RENEGOTIATION)
#error TLS renegotiation is forbidden
#endif
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
#error TLS session tickets are forbidden
#endif
#if defined(MBEDTLS_SSL_CACHE_C) || defined(MBEDTLS_SSL_TICKET_C) || \
    defined(MBEDTLS_SSL_COOKIE_C)
#error TLS session cache/ticket/cookie paths are forbidden
#endif
#if defined(MBEDTLS_SSL_ALPN)
#error ALPN is forbidden
#endif
#if defined(MBEDTLS_SSL_KEYING_MATERIAL_EXPORT)
#error keying-material export is forbidden
#endif
#if defined(MBEDTLS_FS_IO)
#error filesystem I/O is forbidden
#endif
#if defined(MBEDTLS_NET_C)
#error socket support is forbidden
#endif
#if defined(MBEDTLS_TIMING_C)
#error timing support is forbidden
#endif
#if defined(MBEDTLS_THREADING_C)
#error threading is forbidden
#endif
#if defined(MBEDTLS_DEBUG_C)
#error debug support is forbidden
#endif
/*
 * ITEM-508 increment 33, the owner's decision of 2026-10-06: RSA exists in
 * this profile for one job - verifying the CA signatures of the chain that
 * ends at the image's ISRG Root X1 anchor - and the contract is positive
 * exactly there.  PKCS#1 v2.1 (PSS/OAEP), RSA key generation and every RSA
 * key this appliance owns stay forbidden; `mbedtls_rsa_init` stays in the
 * forbidden-symbol list above, so our sources verify through `pk`/`x509` and
 * never build an RSA context of their own.
 */
#if !defined(MBEDTLS_RSA_C) || !defined(MBEDTLS_PKCS1_V15)
#error MBEDTLS_RSA_C and MBEDTLS_PKCS1_V15 are required for CA chain verification
#endif
#if defined(MBEDTLS_PKCS1_V21)
#error PKCS1 v2.1 (PSS/OAEP) is forbidden
#endif
#if defined(MBEDTLS_GENPRIME)
#error RSA key generation is forbidden
#endif
#if defined(MBEDTLS_MD5_C) || defined(MBEDTLS_RIPEMD160_C) || \
    defined(MBEDTLS_SHA1_C) || defined(MBEDTLS_SHA224_C) || \
    defined(MBEDTLS_SHA384_C) || defined(MBEDTLS_SHA512_C) || \
    defined(MBEDTLS_SHA3_C)
#error unused hash algorithms are forbidden
#endif
#if defined(MBEDTLS_CCM_C) || defined(MBEDTLS_CHACHA20_C) || \
    defined(MBEDTLS_CHACHAPOLY_C) || defined(MBEDTLS_POLY1305_C) || \
    defined(MBEDTLS_ARIA_C) || defined(MBEDTLS_CAMELLIA_C) || \
    defined(MBEDTLS_DES_C)
#error unused cipher algorithms are forbidden
#endif
#if defined(MBEDTLS_PEM_PARSE_C) || defined(MBEDTLS_PEM_WRITE_C) || \
    defined(MBEDTLS_BASE64_C)
#error PEM/Base64 paths are forbidden; DER is the frozen format
#endif
/*
 * Item 508 increment 22: the X.509 write side is a positive contract.  The
 * first-boot self-signed certificate (item 386) and the ACME finalize CSR
 * (item 508) are its only callers, and no request path reaches it.  The
 * profile must carry the writer and its prerequisites; parsing a CSR or a CRL
 * stays forbidden verbatim below.
 */
#if !defined(MBEDTLS_X509_CREATE_C)
#error MBEDTLS_X509_CREATE_C is required for the X.509 write side
#endif
#if !defined(MBEDTLS_X509_CRT_WRITE_C)
#error MBEDTLS_X509_CRT_WRITE_C is required for the first-boot certificate
#endif
#if !defined(MBEDTLS_X509_CSR_WRITE_C)
#error MBEDTLS_X509_CSR_WRITE_C is required for the ACME finalize CSR
#endif
#if !defined(MBEDTLS_PK_WRITE_C)
#error MBEDTLS_PK_WRITE_C is required for the X.509 write side
#endif
#if defined(MBEDTLS_X509_CRL_PARSE_C) || defined(MBEDTLS_X509_CSR_PARSE_C)
#error X.509 CRL/CSR parse paths are forbidden
#endif
#if defined(MBEDTLS_PSA_CRYPTO_C) || defined(MBEDTLS_PSA_CRYPTO_STORAGE_C) || \
    defined(MBEDTLS_PSA_CRYPTO_ITS_FILE_C)
#error PSA crypto implementation/storage paths are forbidden
#endif
EOF

if ! cc -fsyntax-only -std=c11 \
    -I"$CONFIG_DIR" -I"$KERNEL_INCLUDE" -I"$MBEDTLS_INCLUDE" \
    -DMBEDTLS_CONFIG_FILE='"hfoc_mbedtls_config.h"' \
    "$WORK/probe.c"; then
    fail "preprocessor/configuration probe rejected the profile"
fi

forbidden_symbols=(
    mbedtls_net_send
    mbedtls_net_recv
    mbedtls_net_connect
    mbedtls_net_accept
    mbedtls_net_bind
    mbedtls_net_listen
    mbedtls_ssl_handshake_client_step
    mbedtls_ssl_handshake_step
    mbedtls_ssl_conf_client_ca_list
    mbedtls_fs_open
    mbedtls_fs_read
    mbedtls_fs_write
    mbedtls_mutex_init
    mbedtls_mutex_lock
    mbedtls_timing_set_delay
    mbedtls_timing_get_delay
    mbedtls_ssl_conf_dbg
    mbedtls_ssl_set_export_keys_cb
    mbedtls_ssl_export_keys_t
    mbedtls_rsa_init
    mbedtls_sha512
    mbedtls_md5
    mbedtls_ripemd160
    mbedtls_ccm_encrypt_and_tag
    mbedtls_chacha20_crypt
)

# Every declared exception must name a source this invocation checks and a
# symbol this verifier forbids; anything else is a refusal.
for index in "${!EXCEPT_SYMBOLS[@]}"; do
    exception_symbol=${EXCEPT_SYMBOLS[$index]}
    exception_source=${EXCEPT_SOURCES[$index]}
    [[ -f "$exception_source" ]] ||
        fail "exception names a missing source: $exception_source"
    exception_listed=false
    for source in "${SYMBOL_SOURCES[@]}"; do
        [[ $source == "$exception_source" ]] && exception_listed=true
    done
    $exception_listed ||
        fail "exception names a source that is not checked: $exception_source"
    exception_forbidden=false
    for symbol in "${forbidden_symbols[@]}"; do
        [[ $symbol == "$exception_symbol" ]] && exception_forbidden=true
    done
    $exception_forbidden ||
        fail "exception names a symbol that is not forbidden: $exception_symbol"
done

for source in "${SYMBOL_SOURCES[@]}"; do
    [[ -f "$source" ]] || fail "missing symbol source: $source"
    for symbol in "${forbidden_symbols[@]}"; do
        excepted=false
        for index in "${!EXCEPT_SYMBOLS[@]}"; do
            if [[ ${EXCEPT_SYMBOLS[$index]} == "$symbol" &&
                  ${EXCEPT_SOURCES[$index]} == "$source" ]]; then
                excepted=true
            fi
        done
        $excepted && continue
        if grep -Fwq "$symbol" "$source"; then
            fail "forbidden mbedTLS symbol '$symbol' in $source"
        fi
    done
done

echo "mbedTLS configuration verification: OK"
