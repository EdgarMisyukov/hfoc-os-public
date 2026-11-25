#!/usr/bin/env bash
# ITEM-508 increment 4: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The request envelope: one account key, ES256, the raw signature a JWS
# carries, and the two encodings the profile does not provide.
#
# Every ACME request is a JWS, so this is the layer the transport will speak
# through.  The claims pinned here:
#
#   * the account key is EC P-256 and nothing else: the profile keeps RSA
#     disabled, the bind refuses any other key type, and the public JWK and the
#     RFC 7638 thumbprint are derived once at bind time from the DER mbedTLS
#     itself writes (`SubjectPublicKeyInfo`) - the module never reaches into
#     mbedTLS's private structures;
#   * the signature is ES256 in the JWS form: the DER mbedTLS writes is
#     converted to the raw R || S pair and a DER that is not exactly two
#     integers of the curve size refuses, so a signature that is not ES256 is
#     never sent; the signing input is `protected || '.' || payload` over the
#     encoded text, and the host proof verifies the signature both with mbedTLS
#     and by rebuilding the DER from the raw pair;
#   * base64url is the unpadded JWS alphabet in both directions: the encoder
#     round-trips every length, and the decoder refuses padding, the standard
#     alphabet's `+` and `/`, a stray character and a length modulo four of
#     one, with an exact output bound (a full 64-byte signature fits exactly);
#   * the thumbprint and the JWK are pinned against values computed outside
#     this tree (OpenSSL for the point, Python for the canonical JSON);
#   * the key material stays inside the module: the private context is static,
#     `free` zeroizes it, and no key byte is ever rendered.
for item508_inc4_file in \
    kernel/include/appliance_acme_jws.h \
    kernel/src/appliance_acme_jws.c \
    tests/appliance-acme-jws-host.c \
    tests/appliance-certificate-fixtures.h \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc4_file ]] || {
        echo "ITEM-508 inc4: отсутствует $item508_inc4_file" >&2
        exit 1
    }
done

# The envelope: ES256, the raw signature, the bounded output.
for item508_inc4_header in \
    'APPLIANCE_ACME_JWS_MAX' \
    'APPLIANCE_ACME_THUMBPRINT_MAX' \
    'APPLIANCE_ACME_SIGNATURE_BYTES 64U' \
    'APPLIANCE_ACME_JWS_BAD_KEY' \
    'APPLIANCE_ACME_JWS_SIGNATURE_FAILED' \
    'appliance_acme_base64url_encode' \
    'appliance_acme_base64url_decode' \
    'appliance_acme_key_import' \
    'appliance_acme_key_generate' \
    'appliance_acme_key_thumbprint' \
    'appliance_acme_jws_sign'; do
    grep -Fq -- "$item508_inc4_header" \
        kernel/include/appliance_acme_jws.h || {
        echo "ITEM-508 inc4: заголовок не публикует $item508_inc4_header" >&2
        exit 1
    }
done
grep -Fq 'MBEDTLS_PK_ECKEY' kernel/src/appliance_acme_jws.c || {
    echo "ITEM-508 inc4: ключ не обязан быть EC" >&2
    exit 1
}
grep -Fq 'MBEDTLS_ECP_DP_SECP256R1' kernel/src/appliance_acme_jws.c || {
    echo "ITEM-508 inc4: ключ не обязан быть P-256" >&2
    exit 1
}
grep -Fq 'MBEDTLS_MD_SHA256' kernel/src/appliance_acme_jws.c || {
    echo "ITEM-508 inc4: подпись не ES256/SHA-256" >&2
    exit 1
}
grep -Fq 'appliance_acme_signature_from_der' kernel/src/appliance_acme_jws.c || {
    echo "ITEM-508 inc4: DER-подпись не преобразуется в raw R||S" >&2
    exit 1
}
if grep -Eq 'malloc|calloc|realloc|fopen|socket|connect\(' \
        kernel/src/appliance_acme_jws.c; then
    echo "ITEM-508 inc4: конверт выделяет память или ходит в сеть" >&2
    exit 1
fi

# The proof: external pins and both directions of the signature check.
for item508_inc4_case in \
    '3ysHIvd-KKLESSZogF4Qw7dQqZjtIvjQ36NVdQDqe_o' \
    'the shared fixture P-256 key imports' \
    'the RFC 7638 thumbprint matches the independent value' \
    'base64url uses its own alphabet and no padding' \
    'padding is refused' \
    'the signature verifies over protected.payload' \
    'the ES256 signature is the raw 64-byte R || S'; do
    grep -Fq -- "$item508_inc4_case" tests/appliance-acme-jws-host.c || {
        echo "ITEM-508 inc4: host-проба не проверяет: $item508_inc4_case" >&2
        exit 1
    }
done

# The build: a TLS-profile kernel object and a host proof in both aggregates.
awk '/^HFOC_TLS_PROFILE_KERNEL_SOURCES :=/,/^$/' Makefile |
    grep -Fq 'kernel/src/appliance_acme_jws.c' || {
    echo "ITEM-508 inc4: конверт не в TLS-профиле сборки" >&2
    exit 1
}
grep -Fq 'test-appliance-acme-jws-host' Makefile || {
    echo "ITEM-508 inc4: Makefile не содержит цель host-пробы" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile line is the literal checked
[[ $(grep -c '$(MAKE) test-appliance-acme-jws-host' Makefile) -ge 2 ]] || {
    echo "ITEM-508 inc4: host-проба не в обоих наборах" >&2
    exit 1
}

# The documents: the increment is recorded where the envelope is read.
grep -Fq 'ITEM-508' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc4: ARCHITECTURE не записывает конверт" >&2
    exit 1
}
grep -Fq 'increment 4' docs/ROADMAP.md || {
    echo "ITEM-508 inc4: ROADMAP не записывает инкремент" >&2
    exit 1
}
