#!/usr/bin/env bash
# ITEM-507 increment 1: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# SNI is enabled in the frozen profile, version 2, and bounded to presentation.
#
# ITEM-507 serves a different certificate per exact DNS name.  The first
# increment changes only the frozen mbedTLS profile: SNI moves from the
# forbidden list to the required server set, the profile version moves 1 -> 2,
# and the verifier replaces `#error SNI is forbidden` with the positive
# contract.  Every other forbidden family stays forbidden verbatim, and the
# profile records that SNI chooses a presentation, never trust or
# authentication.  The name -> certificate store, the selection callback with
# its fail-closed default, the per-connection record and the panel page are the
# following increments; this slice pins that no selection is claimed yet.
for item507_inc1_file in \
    kernel/include/hfoc_mbedtls_config.h \
    scripts/verify-mbedtls-config.sh \
    scripts/check.d/01-lwip-source-license-lock.sh \
    tests/test-mbedtls-config-host.sh \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/hfoc-management-tls-lifecycle.md; do
    [[ -f $item507_inc1_file ]] || {
        echo "ITEM-507 inc1: отсутствует $item507_inc1_file" >&2
        exit 1
    }
done

# The profile: the live version (ITEM-507 took it to 2; ITEM-508 increment 1
# took it to 3 for the ACME client role, and increment 22 took it to 4 for the
# ACME CSR write side), SNI defined, the boundary named, the rest of the
# forbidden families untouched, and the old `#undef` gone.
grep -Fq '#define HFOC_MBEDTLS_CONFIG_VERSION 5' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "ITEM-507 inc1: живой профиль не на версии 4" >&2
    exit 1
}
grep -Fq '#define MBEDTLS_SSL_SERVER_NAME_INDICATION' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "ITEM-507 inc1: профиль не включает SNI" >&2
    exit 1
}
if grep -Fq '#undef MBEDTLS_SSL_SERVER_NAME_INDICATION' \
        kernel/include/hfoc_mbedtls_config.h; then
    echo "ITEM-507 inc1: профиль всё ещё запрещает SNI" >&2
    exit 1
fi
# ITEM-508 increment 1 removed `#undef MBEDTLS_SSL_CLI_C` from this list on
# purpose: the client role is now required for the ACME module.  Every other
# boundary below is unchanged.
for item507_inc1_boundary in \
    'it is never trust, authentication, or an authorization decision' \
    '#undef MBEDTLS_SSL_ALPN' \
    '#undef MBEDTLS_SSL_PROTO_TLS1_2' \
    '#undef MBEDTLS_SSL_SESSION_TICKETS'; do
    grep -Fq -- "$item507_inc1_boundary" \
        kernel/include/hfoc_mbedtls_config.h || {
        echo "ITEM-507 inc1: граница профиля не названа:" \
            "$item507_inc1_boundary" >&2
        exit 1
    }
done

# The verifier: the version pin moved, SNI is required, ALPN is still refused,
# and the old forbidden sentence is gone.
grep -Fq '#error HFOC_MBEDTLS_CONFIG_VERSION must be 5' \
    scripts/verify-mbedtls-config.sh || {
    echo "ITEM-507 inc1: верификатор не пинит живую версию 4" >&2
    exit 1
}
grep -Fq '#error MBEDTLS_SSL_SERVER_NAME_INDICATION is required' \
    scripts/verify-mbedtls-config.sh || {
    echo "ITEM-507 inc1: верификатор не требует SNI" >&2
    exit 1
}
grep -Fq '#error ALPN is forbidden' \
    scripts/verify-mbedtls-config.sh || {
    echo "ITEM-507 inc1: верификатор больше не запрещает ALPN" >&2
    exit 1
}
if grep -Fq '#error SNI is forbidden' scripts/verify-mbedtls-config.sh; then
    echo "ITEM-507 inc1: верификатор всё ещё запрещает SNI" >&2
    exit 1
fi

# The item 262 contract slice pins the same version and the new define.
grep -Fq '#define HFOC_MBEDTLS_CONFIG_VERSION 5' \
    scripts/check.d/01-lwip-source-license-lock.sh || {
    echo "ITEM-507 inc1: срез 01 не пинит живую версию 4" >&2
    exit 1
}
grep -Fq '#define MBEDTLS_SSL_SERVER_NAME_INDICATION' \
    scripts/check.d/01-lwip-source-license-lock.sh || {
    echo "ITEM-507 inc1: срез 01 не пинит SNI" >&2
    exit 1
}

# The host proof: the profile with SNI removed and the stale version are
# refused, ALPN stays refused, and the fixtures exist in the test.
for item507_inc1_proof in \
    'disabled-sni' \
    'enabled-alpn' \
    'stale-version' \
    "expect_fail 'disabled mbedTLS SNI'" \
    "expect_fail 'enabled mbedTLS ALPN'" \
    "expect_fail 'stale mbedTLS config version'"; do
    grep -Fq -- "$item507_inc1_proof" tests/test-mbedtls-config-host.sh || {
        echo "ITEM-507 inc1: проба профиля не содержит:" \
            "$item507_inc1_proof" >&2
        exit 1
    }
done

# The documents: the live profile section names version 2 and SNI, the TLS
# lifecycle says SNI is defined, and the roadmap records the increment.
grep -Fq 'HFOC_MBEDTLS_CONFIG_VERSION 5' docs/ARCHITECTURE.md || {
    echo "ITEM-507 inc1: ARCHITECTURE не называет живую версию 4" >&2
    exit 1
}
grep -Fq 'ITEM-507 increment 1 defined' \
    docs/hfoc-management-tls-lifecycle.md || {
    echo "ITEM-507 inc1: TLS lifecycle не называет включение SNI" >&2
    exit 1
}
grep -Fq 'ITEM-507 increment 1' docs/ROADMAP.md || {
    echo "ITEM-507 inc1: ROADMAP не записывает инкремент" >&2
    exit 1
}
