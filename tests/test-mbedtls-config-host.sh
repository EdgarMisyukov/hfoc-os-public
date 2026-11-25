#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

CONFIG="$ROOT/kernel/include/hfoc_mbedtls_config.h"
VERIFY="$ROOT/scripts/verify-mbedtls-config.sh"
TEST_DIR=$(mktemp -d)

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

expect_fail() {
    local label=$1
    shift
    if "$@" >/dev/null 2>&1; then
        echo "$label unexpectedly succeeded" >&2
        exit 1
    fi
}

verify() {
    bash "$VERIFY" "$@"
}

make_fixture() {
    local target=$1
    mkdir -p "$target"
    cp "$CONFIG" "$target/hfoc_mbedtls_config.h"
}

add_define() {
    local target=$1
    local macro=$2
    sed -i "/^#endif/i #define ${macro}" \
        "$target/hfoc_mbedtls_config.h"
}

remove_define() {
    local target=$1
    local macro=$2
    sed -i "/^#define ${macro}[[:space:]]/d; /^#define ${macro}[[:space:]]*$/d" \
        "$target/hfoc_mbedtls_config.h"
}

set_version() {
    local target=$1
    local version=$2
    sed -i "s/^#define HFOC_MBEDTLS_CONFIG_VERSION .*/#define HFOC_MBEDTLS_CONFIG_VERSION ${version}/" \
        "$target/hfoc_mbedtls_config.h"
}

verify "$CONFIG"

make_fixture "$TEST_DIR/enabled-tls12"
add_define "$TEST_DIR/enabled-tls12" MBEDTLS_SSL_PROTO_TLS1_2
expect_fail 'enabled mbedTLS TLS 1.2' verify \
    "$TEST_DIR/enabled-tls12/hfoc_mbedtls_config.h"

# ITEM-508: the client role is required now - the ACME module is its only
# caller - so the probe that used to reject an enabled client rejects a
# profile without one instead.
make_fixture "$TEST_DIR/disabled-client"
remove_define "$TEST_DIR/disabled-client" MBEDTLS_SSL_CLI_C
expect_fail 'disabled mbedTLS client mode' verify \
    "$TEST_DIR/disabled-client/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-alpn"
add_define "$TEST_DIR/enabled-alpn" MBEDTLS_SSL_ALPN
expect_fail 'enabled mbedTLS ALPN' verify \
    "$TEST_DIR/enabled-alpn/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-rsa"
add_define "$TEST_DIR/enabled-rsa" MBEDTLS_RSA_C
add_define "$TEST_DIR/enabled-rsa" MBEDTLS_PKCS1_V21
expect_fail 'enabled mbedTLS RSA' verify \
    "$TEST_DIR/enabled-rsa/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-sha512"
add_define "$TEST_DIR/enabled-sha512" MBEDTLS_SHA512_C
expect_fail 'enabled mbedTLS SHA-512' verify \
    "$TEST_DIR/enabled-sha512/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-second-curve"
add_define "$TEST_DIR/enabled-second-curve" MBEDTLS_ECP_DP_SECP384R1_ENABLED
expect_fail 'enabled mbedTLS second curve' verify \
    "$TEST_DIR/enabled-second-curve/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-psk"
add_define "$TEST_DIR/enabled-psk" \
    MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
expect_fail 'enabled mbedTLS TLS 1.3 PSK mode' verify \
    "$TEST_DIR/enabled-psk/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-files"
add_define "$TEST_DIR/enabled-files" MBEDTLS_FS_IO
expect_fail 'enabled mbedTLS filesystem I/O' verify \
    "$TEST_DIR/enabled-files/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-sockets"
add_define "$TEST_DIR/enabled-sockets" MBEDTLS_NET_C
expect_fail 'enabled mbedTLS sockets' verify \
    "$TEST_DIR/enabled-sockets/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-threads"
add_define "$TEST_DIR/enabled-threads" MBEDTLS_THREADING_C
expect_fail 'enabled mbedTLS threading' verify \
    "$TEST_DIR/enabled-threads/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-debug"
add_define "$TEST_DIR/enabled-debug" MBEDTLS_DEBUG_C
expect_fail 'enabled mbedTLS debug' verify \
    "$TEST_DIR/enabled-debug/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-key-export"
add_define "$TEST_DIR/enabled-key-export" MBEDTLS_SSL_KEYING_MATERIAL_EXPORT
expect_fail 'enabled mbedTLS keying-material export' verify \
    "$TEST_DIR/enabled-key-export/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/disabled-tls13"
remove_define "$TEST_DIR/disabled-tls13" MBEDTLS_SSL_PROTO_TLS1_3
expect_fail 'disabled mbedTLS TLS 1.3' verify \
    "$TEST_DIR/disabled-tls13/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/disabled-server"
remove_define "$TEST_DIR/disabled-server" MBEDTLS_SSL_SRV_C
expect_fail 'disabled mbedTLS server mode' verify \
    "$TEST_DIR/disabled-server/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/disabled-sni"
remove_define "$TEST_DIR/disabled-sni" MBEDTLS_SSL_SERVER_NAME_INDICATION
expect_fail 'disabled mbedTLS SNI' verify \
    "$TEST_DIR/disabled-sni/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/disabled-ecdsa"
remove_define "$TEST_DIR/disabled-ecdsa" MBEDTLS_ECDSA_C
expect_fail 'disabled mbedTLS ECDSA' verify \
    "$TEST_DIR/disabled-ecdsa/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/disabled-x509"
remove_define "$TEST_DIR/disabled-x509" MBEDTLS_X509_CRT_PARSE_C
expect_fail 'disabled mbedTLS X.509 parsing' verify \
    "$TEST_DIR/disabled-x509/hfoc_mbedtls_config.h"

# ITEM-508 increment 22: the X.509 write side is a positive contract (the
# first-boot certificate and the ACME CSR need it), while parsing a CSR or a
# CRL stays forbidden.
make_fixture "$TEST_DIR/disabled-csr-write"
remove_define "$TEST_DIR/disabled-csr-write" MBEDTLS_X509_CSR_WRITE_C
expect_fail 'disabled mbedTLS CSR writer' verify \
    "$TEST_DIR/disabled-csr-write/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-csr-parse"
add_define "$TEST_DIR/enabled-csr-parse" MBEDTLS_X509_CSR_PARSE_C
expect_fail 'enabled mbedTLS CSR parsing' verify \
    "$TEST_DIR/enabled-csr-parse/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/enabled-crl-parse"
add_define "$TEST_DIR/enabled-crl-parse" MBEDTLS_X509_CRL_PARSE_C
expect_fail 'enabled mbedTLS CRL parsing' verify \
    "$TEST_DIR/enabled-crl-parse/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/disabled-drbg"
remove_define "$TEST_DIR/disabled-drbg" MBEDTLS_CTR_DRBG_C
expect_fail 'disabled mbedTLS CTR-DRBG' verify \
    "$TEST_DIR/disabled-drbg/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/missing-version"
remove_define "$TEST_DIR/missing-version" HFOC_MBEDTLS_CONFIG_VERSION
expect_fail 'missing mbedTLS config version' verify \
    "$TEST_DIR/missing-version/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/stale-version"
set_version "$TEST_DIR/stale-version" 1
expect_fail 'stale mbedTLS config version' verify \
    "$TEST_DIR/stale-version/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/missing-platform-memory"
remove_define "$TEST_DIR/missing-platform-memory" MBEDTLS_PLATFORM_MEMORY
expect_fail 'missing mbedTLS platform memory' verify \
    "$TEST_DIR/missing-platform-memory/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/missing-calloc-macro"
remove_define "$TEST_DIR/missing-calloc-macro" MBEDTLS_PLATFORM_CALLOC_MACRO
expect_fail 'missing mbedTLS calloc macro' verify \
    "$TEST_DIR/missing-calloc-macro/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/missing-free-macro"
remove_define "$TEST_DIR/missing-free-macro" MBEDTLS_PLATFORM_FREE_MACRO
expect_fail 'missing mbedTLS free macro' verify \
    "$TEST_DIR/missing-free-macro/hfoc_mbedtls_config.h"

make_fixture "$TEST_DIR/missing-no-std"
remove_define "$TEST_DIR/missing-no-std" MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
expect_fail 'missing mbedTLS no-std-functions' verify \
    "$TEST_DIR/missing-no-std/hfoc_mbedtls_config.h"

SYMBOL_DIR="$TEST_DIR/symbols"
mkdir -p "$SYMBOL_DIR"

cat >"$SYMBOL_DIR/sockets.c" <<'EOF'
int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return 0;
}
EOF
expect_fail 'forbidden mbedTLS socket symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/sockets.c"

cat >"$SYMBOL_DIR/client.c" <<'EOF'
int mbedtls_ssl_handshake_client_step(void *ssl)
{
    (void)ssl;
    return 0;
}
EOF
expect_fail 'forbidden mbedTLS client symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/client.c"

cat >"$SYMBOL_DIR/files.c" <<'EOF'
int mbedtls_fs_open(const char *path, int flags)
{
    (void)path;
    (void)flags;
    return 0;
}
EOF
expect_fail 'forbidden mbedTLS filesystem symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/files.c"

cat >"$SYMBOL_DIR/threads.c" <<'EOF'
void mbedtls_mutex_lock(void *mutex)
{
    (void)mutex;
}
EOF
expect_fail 'forbidden mbedTLS threading symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/threads.c"

cat >"$SYMBOL_DIR/debug.c" <<'EOF'
void mbedtls_ssl_conf_dbg(void *conf, void *f_dbg, void *p_dbg)
{
    (void)conf;
    (void)f_dbg;
    (void)p_dbg;
}
EOF
expect_fail 'forbidden mbedTLS debug symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/debug.c"

cat >"$SYMBOL_DIR/export.c" <<'EOF'
void mbedtls_ssl_set_export_keys_cb(void *ssl, void *cb, void *ctx)
{
    (void)ssl;
    (void)cb;
    (void)ctx;
}
EOF
expect_fail 'forbidden mbedTLS secret-export symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/export.c"

cat >"$SYMBOL_DIR/rsa.c" <<'EOF'
void mbedtls_rsa_init(void *rsa)
{
    (void)rsa;
}
EOF
expect_fail 'forbidden mbedTLS RSA symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/rsa.c"

cat >"$SYMBOL_DIR/sha512.c" <<'EOF'
void mbedtls_sha512(const unsigned char *input, size_t ilen,
                    unsigned char *output, int is384)
{
    (void)input;
    (void)ilen;
    (void)output;
    (void)is384;
}
EOF
expect_fail 'forbidden mbedTLS SHA-512 symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/sha512.c"

cat >"$SYMBOL_DIR/ccm.c" <<'EOF'
void mbedtls_ccm_encrypt_and_tag(void *ctx, const unsigned char *nonce,
                                 size_t nonce_len, const unsigned char *aad,
                                 size_t aad_len, const unsigned char *input,
                                 unsigned char *output,
                                 unsigned char *tag, size_t tag_len)
{
    (void)ctx;
    (void)nonce;
    (void)nonce_len;
    (void)aad;
    (void)aad_len;
    (void)input;
    (void)output;
    (void)tag;
    (void)tag_len;
}
EOF
expect_fail 'forbidden mbedTLS CCM symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/ccm.c"

cat >"$SYMBOL_DIR/chacha.c" <<'EOF'
void mbedtls_chacha20_crypt(const unsigned char *key,
                            const unsigned char *nonce,
                            uint32_t counter, size_t data_len,
                            const unsigned char *input,
                            unsigned char *output)
{
    (void)key;
    (void)nonce;
    (void)counter;
    (void)data_len;
    (void)input;
    (void)output;
}
EOF
expect_fail 'forbidden mbedTLS ChaCha20 symbol' verify "$CONFIG" \
    "$SYMBOL_DIR/chacha.c"

# ITEM-508 increment 12: one forbidden symbol may be excepted for exactly one
# named source, and a dangling exception is refused rather than ignored.
cat >"$SYMBOL_DIR/client-other.c" <<'EOF'
int mbedtls_ssl_handshake_client_step(void *ssl)
{
    (void)ssl;
    return 0;
}
EOF
verify "$CONFIG" \
    --except=mbedtls_ssl_handshake_client_step:"$SYMBOL_DIR/client.c" \
    "$SYMBOL_DIR/client.c"
echo 'the excepted source passes'
expect_fail 'client symbol in a source without its own exception' verify \
    "$CONFIG" \
    --except=mbedtls_ssl_handshake_client_step:"$SYMBOL_DIR/client.c" \
    "$SYMBOL_DIR/client.c" "$SYMBOL_DIR/client-other.c"
cat >"$SYMBOL_DIR/mixed.c" <<'EOF'
int mbedtls_ssl_handshake_client_step(void *ssl)
{
    (void)ssl;
    return 0;
}
int mbedtls_net_send(void)
{
    return 0;
}
EOF
expect_fail 'another forbidden symbol in the excepted source' verify "$CONFIG" \
    --except=mbedtls_ssl_handshake_client_step:"$SYMBOL_DIR/mixed.c" \
    "$SYMBOL_DIR/mixed.c"
cat >"$SYMBOL_DIR/plain.c" <<'EOF'
int hfoc_plain_source;
EOF
expect_fail 'exception for a source that is not checked' verify "$CONFIG" \
    --except=mbedtls_ssl_handshake_client_step:"$SYMBOL_DIR/plain.c" \
    "$SYMBOL_DIR/client.c"
expect_fail 'exception for a source that does not exist' verify "$CONFIG" \
    --except=mbedtls_ssl_handshake_client_step:"$SYMBOL_DIR/missing.c" \
    "$SYMBOL_DIR/client.c"
expect_fail 'exception for a symbol that is not forbidden' verify "$CONFIG" \
    --except=mbedtls_ssl_read:"$SYMBOL_DIR/client.c" \
    "$SYMBOL_DIR/client.c"
expect_fail 'a malformed exception' verify "$CONFIG" \
    --except=mbedtls_ssl_handshake_client_step \
    "$SYMBOL_DIR/client.c"

echo 'mbedTLS configuration host test: OK'
