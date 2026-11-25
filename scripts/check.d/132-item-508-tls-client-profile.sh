#!/usr/bin/env bash
# ITEM-508 increment 1: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The frozen profile carries the client role the ACME module needs, and every
# other family it used to reject is still rejected verbatim.
#
# ITEM-503 built the operator's own certificate import; ITEM-504/507 made a
# name select a certificate and gave the panel the mutation.  This increment
# takes the first step of the item that lets the appliance obtain its own
# certificate: `MBEDTLS_SSL_CLI_C` is defined and the profile version moves to
# 3 (increment 22 later moves the live version to 4 for the CSR write side,
# and this slice follows the live pin), and the verifier's old `#error mbedTLS
# client mode is forbidden` becomes the
# positive contract.  It is deliberately not a general TLS client - the ACME
# module is the only caller the item allows, the trust anchor is a named DER
# root in the image, the hostname is verified, and RSA and PEM stay off - so
# the claims pinned here are:
#
#   * the profile defines the client role and still defines the server role,
#     and every protocol, platform and algorithm family the item keeps
#     forbidden is still `#undef`ed exactly as before (TLS 1.2, DTLS,
#     renegotiation, tickets, cache/ticket/cookie, ALPN, FS_IO, NET_C,
#     TIMING_C, RSA, PEM);
#   * the verifier requires the client role, still requires the server role,
#     and no longer carries the "client mode is forbidden" refusal;
#   * the per-source forbidden-symbol check keeps the client handshake
#     symbols out of the sources it lists, so enabling the profile cannot
#     quietly make another translation unit a client;
#   * the host proof rejects a profile without the client role instead of
#     rejecting one with it, and the documents say what the client role is
#     for and what it is not.
for item508_inc1_file in \
    kernel/include/hfoc_mbedtls_config.h \
    scripts/verify-mbedtls-config.sh \
    tests/test-mbedtls-config-host.sh \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc1_file ]] || {
        echo "ITEM-508 inc1: отсутствует $item508_inc1_file" >&2
        exit 1
    }
done

# The profile: client and server present, version 4, every other rejection
# untouched.
grep -Fq '#define HFOC_MBEDTLS_CONFIG_VERSION 5' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "ITEM-508 inc1: профиль не перешёл на версию 4" >&2
    exit 1
}
for item508_inc1_define in \
    '#define MBEDTLS_SSL_CLI_C' \
    '#define MBEDTLS_SSL_SRV_C' \
    '#define MBEDTLS_SSL_TLS_C' \
    '#define MBEDTLS_SSL_PROTO_TLS1_3'; do
    grep -Fq -- "$item508_inc1_define" \
        kernel/include/hfoc_mbedtls_config.h || {
        echo "ITEM-508 inc1: профиль не определяет $item508_inc1_define" >&2
        exit 1
    }
done
for item508_inc1_undef in \
    '#undef MBEDTLS_SSL_PROTO_TLS1_2' \
    '#undef MBEDTLS_SSL_PROTO_DTLS' \
    '#undef MBEDTLS_SSL_RENEGOTIATION' \
    '#undef MBEDTLS_SSL_SESSION_TICKETS' \
    '#undef MBEDTLS_SSL_CACHE_C' \
    '#undef MBEDTLS_SSL_COOKIE_C' \
    '#undef MBEDTLS_SSL_ALPN' \
    '#undef MBEDTLS_FS_IO' \
    '#undef MBEDTLS_NET_C' \
    '#undef MBEDTLS_TIMING_C' \
    '#undef MBEDTLS_PEM_PARSE_C'; do
    grep -Fq -- "$item508_inc1_undef" \
        kernel/include/hfoc_mbedtls_config.h || {
        echo "ITEM-508 inc1: профиль больше не запрещает $item508_inc1_undef" >&2
        exit 1
    }
done
# ITEM-508 increment 33 moved RSA from "forbidden" to "verification only", at
# the owner's decision: the two defines are present, and PKCS#1 v2.1, key
# generation and the RSA context of our own stay forbidden.  Slice 160 pins
# the whole positive contract.
grep -Fq '#define MBEDTLS_RSA_C' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "ITEM-508 inc1: профиль не несёт проверку RSA" >&2
    exit 1
}
if grep -Fq '#undef MBEDTLS_SSL_CLI_C' \
        kernel/include/hfoc_mbedtls_config.h; then
    echo "ITEM-508 inc1: профиль всё ещё снимает клиентскую роль" >&2
    exit 1
fi

# The verifier: a positive contract for the client, the server still required,
# and the old refusal gone.
grep -Fq '#if HFOC_MBEDTLS_CONFIG_VERSION != 5' \
    scripts/verify-mbedtls-config.sh || {
    echo "ITEM-508 inc1: верификатор не пинит версию 4" >&2
    exit 1
}
grep -Fq '#error MBEDTLS_SSL_CLI_C is required for the ACME client role' \
    scripts/verify-mbedtls-config.sh || {
    echo "ITEM-508 inc1: верификатор не требует клиентскую роль" >&2
    exit 1
}
grep -Fq '#error MBEDTLS_SSL_SRV_C is required' \
    scripts/verify-mbedtls-config.sh || {
    echo "ITEM-508 inc1: верификатор больше не требует серверную роль" >&2
    exit 1
}
if grep -Fq '#error mbedTLS client mode is forbidden' \
        scripts/verify-mbedtls-config.sh; then
    echo "ITEM-508 inc1: верификатор всё ещё запрещает клиентскую роль" >&2
    exit 1
fi
# The item's point 5: the client path belongs to the ACME module alone.  Until
# that module lands, the per-source symbol check keeps the client handshake
# symbols forbidden in every source it is given.
for item508_inc1_symbol in \
    mbedtls_ssl_handshake_client_step \
    mbedtls_ssl_conf_client_ca_list; do
    grep -Fq -- "$item508_inc1_symbol" \
        scripts/verify-mbedtls-config.sh || {
        echo "ITEM-508 inc1: запрещённый символ $item508_inc1_symbol не пинится" \
            >&2
        exit 1
    }
done

# The host proof: a profile without the client role is the failure case now.
grep -Fq 'disabled mbedTLS client mode' tests/test-mbedtls-config-host.sh || {
    echo "ITEM-508 inc1: host-проба не отвергает профиль без клиента" >&2
    exit 1
}
if grep -Fq "enabled mbedTLS client mode" tests/test-mbedtls-config-host.sh; then
    echo "ITEM-508 inc1: host-проба всё ещё отвергает клиентскую роль" >&2
    exit 1
fi

# The documents: the client role's reason and its limit are recorded where the
# profile is read.
grep -Fq 'ITEM-508' README.md || {
    echo "ITEM-508 inc1: README не записывает клиентскую роль" >&2
    exit 1
}
grep -Fq 'HFOC_MBEDTLS_CONFIG_VERSION 5' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc1: ARCHITECTURE не записывает версию 4" >&2
    exit 1
}
grep -Fq 'ITEM-508' docs/ROADMAP.md || {
    echo "ITEM-508 inc1: ROADMAP не записывает инкремент" >&2
    exit 1
}
