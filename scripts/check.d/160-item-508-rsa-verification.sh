#!/usr/bin/env bash
# ITEM-508 increment 33: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal Makefile and shell text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# RSA verification only, for the real Let's Encrypt chain.
#
# The owner decided on 2026-10-06: enable `MBEDTLS_RSA_C` and
# `MBEDTLS_PKCS1_V15` so the X.509 path can verify the CA signatures of the
# chain that ends at the image's ISRG Root X1 anchor (ISRG Root X1 is RSA-4096,
# R10/R11 are RSA).  Nothing else becomes RSA: PKCS#1 v2.1, key generation and
# every RSA key this appliance owns stay off, `mbedtls_rsa_init` stays
# forbidden in our sources, and the proof verifies the real certificates the
# anchor actually signed instead of the old `ANCHOR_UNAVAILABLE`.
for item508_inc33_file in \
    kernel/include/hfoc_mbedtls_config.h \
    scripts/verify-mbedtls-config.sh \
    tests/hfoc-mbedtls-manifest-host-config.h \
    tests/appliance-acme-le-chain-fixtures.h \
    tests/appliance-acme-trust-host.c \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc33_file ]] || {
        echo "ITEM-508 inc33: отсутствует $item508_inc33_file" >&2
        exit 1
    }
done

# 1. The frozen profile: version 5, RSA verification defined, PKCS#1 v2.1 and
#    key generation still off, and no RSA undef left behind.
for item508_inc33_profile_pin in \
    '#define HFOC_MBEDTLS_CONFIG_VERSION 5' \
    '#define MBEDTLS_RSA_C' \
    '#define MBEDTLS_PKCS1_V15' \
    '#undef MBEDTLS_PKCS1_V21'; do
    grep -Fq -- "$item508_inc33_profile_pin" \
        kernel/include/hfoc_mbedtls_config.h || {
        echo "ITEM-508 inc33: профиль не содержит «$item508_inc33_profile_pin»" \
            >&2
        exit 1
    }
done
for item508_inc33_profile_gone in \
    '#undef MBEDTLS_RSA_C' \
    '#undef MBEDTLS_PKCS1_V15'; do
    if grep -Fq -- "$item508_inc33_profile_gone" \
        kernel/include/hfoc_mbedtls_config.h; then
        echo "ITEM-508 inc33: профиль всё ещё снимает «$item508_inc33_profile_gone»" \
            >&2
        exit 1
    fi
done
grep -Fq '2026-10-06' kernel/include/hfoc_mbedtls_config.h || {
    echo "ITEM-508 inc33: профиль не называет решение владельца" >&2
    exit 1
}

# 2. The verifier: the version moves, the RSA refusal becomes a positive
#    contract, and the symbol control still forbids an RSA context of our own.
for item508_inc33_verifier_pin in \
    '#if HFOC_MBEDTLS_CONFIG_VERSION != 5' \
    '#error HFOC_MBEDTLS_CONFIG_VERSION must be 5' \
    '#error MBEDTLS_RSA_C and MBEDTLS_PKCS1_V15 are required for CA chain verification' \
    '#error PKCS1 v2.1 (PSS/OAEP) is forbidden' \
    '#error RSA key generation is forbidden' \
    'mbedtls_rsa_init'; do
    grep -Fq -- "$item508_inc33_verifier_pin" \
        scripts/verify-mbedtls-config.sh || {
        echo "ITEM-508 inc33: верификатор не содержит «$item508_inc33_verifier_pin»" \
            >&2
        exit 1
    }
done
if grep -Fq '#error RSA/PKCS1 is forbidden' scripts/verify-mbedtls-config.sh; then
    echo "ITEM-508 inc33: верификатор всё ещё запрещает RSA целиком" >&2
    exit 1
fi

# 3. The host proof profile mirrors the frozen profile's RSA contract, and the
#    manifest build carries the RSA sources the X.509 path needs.
for item508_inc33_host_pin in \
    '#define MBEDTLS_RSA_C' \
    '#define MBEDTLS_PKCS1_V15'; do
    grep -Fq -- "$item508_inc33_host_pin" \
        tests/hfoc-mbedtls-manifest-host-config.h || {
        echo "ITEM-508 inc33: host-профиль не содержит «$item508_inc33_host_pin»" \
            >&2
        exit 1
    }
done
for item508_inc33_make_pin in \
    'third_party/mbedtls/library/rsa.c' \
    'third_party/mbedtls/library/rsa_alt_helpers.c'; do
    grep -Fq -- "$item508_inc33_make_pin" Makefile || {
        echo "ITEM-508 inc33: Makefile не несёт «$item508_inc33_make_pin»" >&2
        exit 1
    }
done

# 4. The fixtures are the real certificates: byte counts and SHA-256 pinned in
#    the header, and the proof verifies them against the image anchor with zero
#    flags while the old refusals still hold.
for item508_inc33_fixture_pin in \
    'LETSENCRYPT_CHAIN_NOW' \
    '9d7c3f1aa6ad2b2ec0d5cf1e246f8d9ae6cbc9fd0755ad37bb974b1f2fb603f3' \
    '591e9ce6c863d3a079e9fabe1478c7339a26b21269dde795211361024ae31a44'; do
    grep -Fq -- "$item508_inc33_fixture_pin" \
        tests/appliance-acme-le-chain-fixtures.h || {
        echo "ITEM-508 inc33: фикстуры не содержат «$item508_inc33_fixture_pin»" \
            >&2
        exit 1
    }
done
for item508_inc33_proof_pin in \
    'static void test_the_image_anchor_verifies_the_real_chain(void)' \
    'the real R10 chains to the image anchor with zero flags' \
    'the real R11 chains to the image anchor with zero flags' \
    'the verified real chain is counted with no flags' \
    'the real chain with a name it does not cover is refused by name' \
    'the real chain outside its window is refused by window' \
    'the real chain under a foreign anchor is rejected, not trusted'; do
    grep -Fq -- "$item508_inc33_proof_pin" \
        tests/appliance-acme-trust-host.c || {
        echo "ITEM-508 inc33: проба не содержит «$item508_inc33_proof_pin»" >&2
        exit 1
    }
done
if grep -Fq 'is not usable while RSA verification is off' \
    tests/appliance-acme-trust-host.c; then
    echo "ITEM-508 inc33: проба всё ещё пинит старый отказ ANCHOR_UNAVAILABLE" \
        >&2
    exit 1
fi

# 5. The documents name the increment and the new version is recorded.
for item508_inc33_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-508 increment 33' "$item508_inc33_doc" || {
        echo "ITEM-508 inc33: $item508_inc33_doc не называет инкремент" >&2
        exit 1
    }
done
grep -Fq 'HFOC_MBEDTLS_CONFIG_VERSION 5' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc33: ARCHITECTURE не называет версию профиля 5" >&2
    exit 1
}
