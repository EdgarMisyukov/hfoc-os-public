#!/usr/bin/env bash
# ITEM-508 increment 5: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The ACME order as a sequence of named steps, transport-free and provable on
# the host.
#
# The transport and the HTTP-01 listener do not exist yet, so the spine has to
# be provable without either: every function consumes a reply the caller
# fetched or produces the request body the caller sends.  The claims pinned
# here:
#
#   * the item's point 6 is enforced first: a clock that is not validated
#     refuses with `BAD_CLOCK` before the directory is read, no request is
#     built, and the refusal is published as a record;
#   * every request is a real JWS - the protected header carries `alg`, the
#     nonce and the exact URL, plus the public JWK while the account is being
#     created and the account `kid` afterwards; the host proof verifies each
#     request's signature over `protected.payload` with the shared fixture
#     key, so "the request is a JWS" is not taken on faith;
#   * the newOrder payload is built from the identifiers the caller declared
#     through `declare_identifiers` (exact DNS names, bounded), never from a
#     wire reply;
#   * the challenge and finalize steps POST the empty JSON object RFC 8555
#     defines, the challenge is polled until it is valid and the order until it
#     names the certificate, and a challenge the CA marked invalid, a finalize
#     before a valid challenge and a valid order without a certificate URL all
#     refuse by name;
#   * the authorization step is POST-as-GET (an empty payload), and the key
#     authorization of its http-01 challenge is the token, a dot, and the RFC
#     7638 thumbprint of the account key - the string the HTTP-01 listener
#     will serve;
#   * refusals are named: an unparsable directory, a non-base64url nonce, an
#     account status that is not valid, an order status RFC 8555 does not
#     define, an authorization without http-01;
#   * the published record carries the state, the step and the status, and it
#     never carries a nonce, a token or a key byte.
for item508_inc5_file in \
    kernel/include/appliance_acme_flow.h \
    kernel/src/appliance_acme_flow.c \
    tests/appliance-acme-flow-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc5_file ]] || {
        echo "ITEM-508 inc5: отсутствует $item508_inc5_file" >&2
        exit 1
    }
done

# The flow's named states, statuses and steps.
for item508_inc5_name in \
    'APPLIANCE_ACME_FLOW_STATE_AWAITING_CLOCK' \
    'APPLIANCE_ACME_FLOW_STATE_ACCOUNT_PENDING' \
    'APPLIANCE_ACME_FLOW_STATE_ORDER_PENDING' \
    'APPLIANCE_ACME_FLOW_STATE_AUTHORIZATION_PENDING' \
    'APPLIANCE_ACME_FLOW_BAD_CLOCK' \
    'APPLIANCE_ACME_FLOW_BAD_NONCE' \
    'APPLIANCE_ACME_FLOW_BAD_STATUS' \
    'APPLIANCE_ACME_FLOW_STEP_KEY_AUTHORIZATION' \
    'appliance_acme_flow_begin' \
    'appliance_acme_flow_declare_identifiers' \
    'appliance_acme_flow_begin_account' \
    'appliance_acme_flow_account_result' \
    'appliance_acme_flow_begin_order' \
    'appliance_acme_flow_order_result' \
    'appliance_acme_flow_begin_authorization' \
    'appliance_acme_flow_authorization_result' \
    'appliance_acme_flow_key_authorization' \
    'APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING' \
    'APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING' \
    'APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY' \
    'APPLIANCE_ACME_FLOW_STEP_CHALLENGE' \
    'APPLIANCE_ACME_FLOW_STEP_FINALIZE' \
    'APPLIANCE_ACME_FLOW_STEP_CERTIFICATE' \
    'appliance_acme_flow_begin_challenge' \
    'appliance_acme_flow_challenge_result' \
    'appliance_acme_flow_begin_finalize' \
    'appliance_acme_flow_finalize_result'; do
    grep -Fq -- "$item508_inc5_name" kernel/include/appliance_acme_flow.h || {
        echo "ITEM-508 inc5: заголовок не публикует $item508_inc5_name" >&2
        exit 1
    }
done

# The clock rule and the key authorization are in the implementation.
grep -Fq 'now_epoch_seconds == 0U' kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc5: непроверенные часы не отвергаются" >&2
    exit 1
}
grep -Fq 'appliance_acme_key_thumbprint' kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc5: key authorization не использует отпечаток ключа" >&2
    exit 1
}
grep -Fq 'appliance_acme_jws_sign' kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc5: запросы не подписываются конвертом" >&2
    exit 1
}
if grep -Eq 'malloc|calloc|realloc|fopen|socket|connect\(' \
        kernel/src/appliance_acme_flow.c; then
    echo "ITEM-508 inc5: поток выделяет память или ходит в сеть" >&2
    exit 1
fi
if grep -Eq 'serial_write|console_write' kernel/src/appliance_acme_flow.c; then
    echo "ITEM-508 inc5: поток печатает сам вместо записи" >&2
    exit 1
fi

# The proof: the record, the clock refusal and the spine.
for item508_inc5_case in \
    'an unvalidated clock refuses the order' \
    'the refusal is published as a record' \
    'the request signature verifies over protected.payload' \
    'the key authorization is the token and the thumbprint' \
    'the record never carries the nonce' \
    'the record never carries the challenge token' \
    'POST-as-GET carries the empty payload' \
    'an order status RFC 8555 does not define refuses the flow' \
    'the challenge POST carries the empty JSON object' \
    'a valid challenge opens finalize' \
    'a valid order names the certificate and ends the flow' \
    'a challenge the CA marked invalid fails the flow' \
    'a valid order without a certificate URL fails the flow' \
    'finalize before a valid challenge refuses'; do
    grep -Fq -- "$item508_inc5_case" tests/appliance-acme-flow-host.c || {
        echo "ITEM-508 inc5: host-проба не проверяет: $item508_inc5_case" >&2
        exit 1
    }
done

# The build: a TLS-profile kernel object and a host proof in both aggregates.
awk '/^HFOC_TLS_PROFILE_KERNEL_SOURCES :=/,/^$/' Makefile |
    grep -Fq 'kernel/src/appliance_acme_flow.c' || {
    echo "ITEM-508 inc5: поток не в TLS-профиле сборки" >&2
    exit 1
}
grep -Fq 'test-appliance-acme-flow-host' Makefile || {
    echo "ITEM-508 inc5: Makefile не содержит цель host-пробы" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile line is the literal checked
[[ $(grep -c '$(MAKE) test-appliance-acme-flow-host' Makefile) -ge 2 ]] || {
    echo "ITEM-508 inc5: host-проба не в обоих наборах" >&2
    exit 1
}

# The documents: the increment is recorded where the order is read.
grep -Fq 'ITEM-508' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc5: ARCHITECTURE не записывает поток заказа" >&2
    exit 1
}
grep -Fq 'increment 5' docs/ROADMAP.md || {
    echo "ITEM-508 inc5: ROADMAP не записывает инкремент" >&2
    exit 1
}
