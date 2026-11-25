#!/usr/bin/env bash
# ITEM-508 increment 8: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The certificate step: the chain the CA returned is decoded and trusted.
#
# The order reaches `CERTIFICATE_READY` with a certificate URL; the body behind
# that URL is a PEM chain RFC 8555 section 7.4.2 defines, and the item does not
# store a certificate it has not verified against the anchor named in the
# image.  This slice pins that step:
#
#   * the body is decoded member by member with the bounded ITEM-503 PEM
#     decoder, into the caller's DER buffer, leaf first; a body that is not a
#     chain, a block that never closes and a chain with more members than the
#     trust module accepts are refusals;
#   * the decoded chain is verified against the anchor the caller bound and the
#     first identifier the order declared, at the very instant handed in: the
#     flow never calls the trust module with a zero clock;
#   * a buffer too small and an unvalidated clock are caller problems and leave
#     the order usable; an untrustworthy chain fails the order;
#   * the published record names the step and the verified state and never
#     carries the certificate or a key byte.
for item508_inc8_file in \
    kernel/include/appliance_acme_flow.h \
    kernel/src/appliance_acme_flow.c \
    tests/appliance-acme-flow-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc8_file ]] || {
        echo "ITEM-508 inc8: отсутствует $item508_inc8_file" >&2
        exit 1
    }
done

# The published state, statuses and the step's entry point.
for item508_inc8_name in \
    'APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_VERIFIED' \
    'APPLIANCE_ACME_FLOW_BAD_CHAIN' \
    'APPLIANCE_ACME_FLOW_UNTRUSTED' \
    'bool certificate_verified' \
    'appliance_acme_flow_certificate_result'; do
    grep -Fq -- "$item508_inc8_name" kernel/include/appliance_acme_flow.h || {
        echo "ITEM-508 inc8: заголовок не публикует $item508_inc8_name" >&2
        exit 1
    }
done

# The step decodes PEM with the bounded decoder and verifies before publishing.
grep -Fq 'pem_decode_block' kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc8: тело сертификата не разбирается PEM-декодером" >&2
    exit 1
}
grep -Fq 'BEGIN CERTIFICATE' kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc8: шаг не ищет блок сертификата" >&2
    exit 1
}
grep -Fq 'appliance_acme_trust_verify' kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc8: цепочка публикуется без проверки доверия" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ACME_TRUST_CHAIN_MEMBERS_MAX' \
    kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc8: число членов цепочки не ограничено" >&2
    exit 1
}
grep -Fq 'now_epoch_seconds == 0U' kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc8: шаг не отвергает непроверенные часы" >&2
    exit 1
}

# The proof: the verified chain, the DER published and every refusal.
for item508_inc8_case in \
    'the fixture anchor binds for the certificate step' \
    'the flow reaches the certificate step' \
    'a missing certificate body refuses' \
    'a body buffer too small refuses and leaves the order usable' \
    'an unvalidated clock refuses the certificate and leaves the order' \
    'the chain the CA returned verifies and its DER is published' \
    'the verified certificate is recorded' \
    'the record never carries the certificate itself' \
    'a second certificate download is out of order' \
    'a refused certificate publishes no DER length' \
    'a chain for another name is untrusted and fails the order' \
    'a chain from another CA is untrusted' \
    'a body that is not a certificate chain fails the order' \
    'a chain outside its validity window is untrusted'; do
    grep -Fq -- "$item508_inc8_case" tests/appliance-acme-flow-host.c || {
        echo "ITEM-508 inc8: host-проба не проверяет: $item508_inc8_case" >&2
        exit 1
    }
done

# The build: the flow proof links the trust module and the PEM decoder.
for item508_inc8_source in \
    'kernel/src/appliance_acme_trust.c' \
    'kernel/src/appliance_acme_trust_anchor.c' \
    'kernel/src/pem_decode.c'; do
    awk '/^test-appliance-acme-flow-host:/,/^[[:space:]]*build\/test-appliance-acme-flow-host/' \
        Makefile | grep -Fq -- "$item508_inc8_source" || {
        echo "ITEM-508 inc8: host-проба потока не линкует $item508_inc8_source" >&2
        exit 1
    }
done
grep -Fq 'HFOC_APPLIANCE_ACME_TRUST_HOST_TEST' Makefile || {
    echo "ITEM-508 inc8: модуль доверия не собран для host-пробы" >&2
    exit 1
}

# The documents: the increment is recorded where the certificate is read.
grep -Fq 'ITEM-508' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc8: ARCHITECTURE не записывает проверку сертификата" >&2
    exit 1
}
grep -Fq 'increment 8' docs/ROADMAP.md || {
    echo "ITEM-508 inc8: ROADMAP не записывает инкремент" >&2
    exit 1
}
