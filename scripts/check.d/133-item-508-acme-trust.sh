#!/usr/bin/env bash
# ITEM-508 increment 2: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The ACME client role's single trust decision, and the one root it may trust.
#
# ITEM-508 increment 1 put the client role into the profile; this increment
# gives it the decision it will verify with: one named root in the image and a
# module that refuses - by name - an unvalidated clock, a name that is not an
# exact DNS name, an anchor that cannot be used, a chain that does not parse or
# is out of its window, a chain that does not verify and a chain that verifies
# for another name.  The claims pinned here:
#
#   * the image carries exactly one anchor, ISRG Root X1 as DER, and the host
#     proof pins its digest - a silent root swap is a failure;
#   * the module is the only place the client-role chain is verified; it
#     carries no network root store and no second anchor;
#   * the refusals are named and counted, and an unvalidated clock refuses
#     before anything is parsed (the item starts no order on such a clock);
#   * the module compiles with the TLS-serving mbedTLS profile, the host proof
#     drives every refusal path with fixture chains and no network, and the
#     proof is part of the host suite;
#   * **the RSA finding is pinned honestly**: ISRG Root X1 is an RSA
#     certificate and the frozen profile carries no RSA verification, so the
#     image anchor cannot be parsed yet - the module answers
#     `ANCHOR_UNAVAILABLE` instead of pretending to verify.  The increment that
#     first verifies a real Let's Encrypt chain has to move this deliberately.
for item508_inc2_file in \
    kernel/include/appliance_acme_trust.h \
    kernel/src/appliance_acme_trust.c \
    kernel/src/appliance_acme_trust_anchor.c \
    tests/appliance-acme-trust-host.c \
    tests/appliance-certificate-fixtures.h \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc2_file ]] || {
        echo "ITEM-508 inc2: отсутствует $item508_inc2_file" >&2
        exit 1
    }
done

# The anchor: one named root, its digest pinned where the proof and the slice
# can both see it, and the DER itself in the image.
grep -Fq '96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6' \
    kernel/src/appliance_acme_trust_anchor.c || {
    echo "ITEM-508 inc2: якорь не пинит отпечаток ISRG Root X1" >&2
    exit 1
}
grep -Fq '96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6' \
    tests/appliance-acme-trust-host.c || {
    echo "ITEM-508 inc2: host-проба не пинит отпечаток якоря" >&2
    exit 1
}
grep -Fq 'appliance_acme_trust_anchor_der[1391U]' \
    kernel/src/appliance_acme_trust_anchor.c || {
    echo "ITEM-508 inc2: DER-якорь не той длины, что измерена" >&2
    exit 1
}
for item508_inc2_anchor_symbol in \
    'appliance_acme_trust_anchor_der_data' \
    'appliance_acme_trust_anchor_der_length'; do
    grep -Fq -- "$item508_inc2_anchor_symbol" \
        kernel/include/appliance_acme_trust.h || {
        echo "ITEM-508 inc2: заголовок не публикует $item508_inc2_anchor_symbol" \
            >&2
        exit 1
    }
done
if grep -Fq 'mbedtls_x509_crt_parse' kernel/src/appliance_acme_trust_anchor.c; then
    echo "ITEM-508 inc2: данные якоря разбираются не модулем доверия" >&2
    exit 1
fi

# The module: named refusals, the clock first, and no other anchor source.
for item508_inc2_status in \
    'APPLIANCE_ACME_TRUST_BAD_CLOCK' \
    'APPLIANCE_ACME_TRUST_BAD_NAME' \
    'APPLIANCE_ACME_TRUST_ANCHOR_UNAVAILABLE' \
    'APPLIANCE_ACME_TRUST_CHAIN_PARSE_FAILED' \
    'APPLIANCE_ACME_TRUST_CHAIN_WINDOW_REJECTED' \
    'APPLIANCE_ACME_TRUST_CHAIN_REJECTED' \
    'APPLIANCE_ACME_TRUST_NAME_REJECTED'; do
    grep -Fq -- "$item508_inc2_status" \
        kernel/include/appliance_acme_trust.h || {
        echo "ITEM-508 inc2: модуль не публикует $item508_inc2_status" >&2
        exit 1
    }
done
grep -Fq 'mbedtls_x509_crt_verify' kernel/src/appliance_acme_trust.c || {
    echo "ITEM-508 inc2: модуль не проверяет цепочку через mbedTLS" >&2
    exit 1
}
grep -Fq 'now_epoch_seconds == 0U' kernel/src/appliance_acme_trust.c || {
    echo "ITEM-508 inc2: модуль не отказывает на непроверенных часах" >&2
    exit 1
}
if grep -Eq 'getenv|fopen|socket|connect\(' kernel/src/appliance_acme_trust.c; then
    echo "ITEM-508 inc2: модуль доверия ходит в сеть или файлы" >&2
    exit 1
fi

# The build: the module is a TLS-profile kernel object and the proof is in the
# host suite.
grep -Fq 'kernel/src/appliance_acme_trust.c' Makefile || {
    echo "ITEM-508 inc2: Makefile не собирает модуль доверия" >&2
    exit 1
}
awk '/^HFOC_TLS_PROFILE_KERNEL_SOURCES :=/,/^$/' Makefile |
    grep -Fq 'kernel/src/appliance_acme_trust.c' || {
    echo "ITEM-508 inc2: модуль доверия не в TLS-профиле сборки" >&2
    exit 1
}
grep -Fq 'test-appliance-acme-trust-host' Makefile || {
    echo "ITEM-508 inc2: Makefile не содержит цель host-пробы" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile line is the literal checked
[[ $(grep -c '$(MAKE) test-appliance-acme-trust-host' Makefile) -ge 2 ]] || {
    echo "ITEM-508 inc2: host-проба не в обоих наборах" >&2
    exit 1
}

# The proof itself: every refusal path is driven, and the chain that really
# reaches the image anchor is asserted rather than hidden.  ITEM-508 increment
# 33 enabled RSA verification, so the anchor is no longer unusable and the
# pinned case moved from the old `ANCHOR_UNAVAILABLE` finding to the real
# Let's Encrypt certificate that now verifies.
for item508_inc2_case in \
    'APPLIANCE_ACME_TRUST_CHAIN_REJECTED' \
    'APPLIANCE_ACME_TRUST_NAME_REJECTED' \
    'APPLIANCE_ACME_TRUST_CHAIN_WINDOW_REJECTED' \
    'APPLIANCE_ACME_TRUST_BAD_NAME' \
    'the real R10 chains to the image anchor with zero flags'; do
    grep -Fq -- "$item508_inc2_case" tests/appliance-acme-trust-host.c || {
        echo "ITEM-508 inc2: host-проба не проверяет $item508_inc2_case" >&2
        exit 1
    }
done

# The documents: the increment is recorded where the trust decision is read.
grep -Fq 'ITEM-508' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc2: ARCHITECTURE не записывает якорь доверия" >&2
    exit 1
}
grep -Fq 'increment 2' docs/ROADMAP.md || {
    echo "ITEM-508 inc2: ROADMAP не записывает инкремент" >&2
    exit 1
}
