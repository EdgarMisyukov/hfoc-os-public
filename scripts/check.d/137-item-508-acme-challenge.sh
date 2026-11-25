#!/usr/bin/env bash
# ITEM-508 increment 7: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The HTTP-01 answer the challenge listener serves, decided without a socket.
#
# The item's point 8 wants a listener that exists only for the task: it answers
# the one well-known path for the token that is currently armed and nothing
# else.  This slice pins the decision that the transport will execute, and the
# request line it may execute it on:
#
#   * the listener reads exactly one request line - `METHOD SP TARGET SP
#     VERSION`, origin-form, HTTP/1.0 or HTTP/1.1, optional CRLF - and a line
#     that is anything else, or that carries a control byte, is `BAD_REQUEST`
#     before any state is consulted;
#   * the only answerable path is the prefix and the armed token byte for byte:
#     no query string, no trailing slash, no extra segment, no case folding, no
#     lookalike prefix, and never another token;
#   * the only answerable methods are GET and HEAD - GET answers the key
#     authorization and HEAD answers no body - and anything else is
#     `METHOD_NOT_ALLOWED`;
#   * nothing is served before a token is armed or after it is disarmed, and
#     nothing is served past the 900 s lifetime or on an unvalidated clock;
#   * the module is transport-free and TLS-free: it allocates nothing, opens
#     nothing, prints nothing and includes no mbedTLS header, so it builds in
#     the ordinary kernel as well as the TLS profile;
#   * the published record is a named status, never a body or a token.
for item508_inc7_file in \
    kernel/include/appliance_acme_challenge.h \
    kernel/src/appliance_acme_challenge.c \
    tests/appliance-acme-challenge-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc7_file ]] || {
        echo "ITEM-508 inc7: отсутствует $item508_inc7_file" >&2
        exit 1
    }
done

# The published constants, statuses, the request line and the entry points.
for item508_inc7_name in \
    'APPLIANCE_ACME_CHALLENGE_PREFIX "/.well-known/acme-challenge/"' \
    'APPLIANCE_ACME_CHALLENGE_LIFETIME_SECONDS 900U' \
    'APPLIANCE_ACME_CHALLENGE_ANSWER_MAX 512U' \
    'APPLIANCE_ACME_CHALLENGE_PATH_MAX' \
    'APPLIANCE_ACME_CHALLENGE_BAD_REQUEST' \
    'APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL' \
    'APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED' \
    'APPLIANCE_ACME_CHALLENGE_NOT_FOUND' \
    'APPLIANCE_ACME_CHALLENGE_EXPIRED' \
    'APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE' \
    'APPLIANCE_ACME_CHALLENGE_BAD_TOKEN' \
    'struct appliance_acme_challenge_request' \
    'struct appliance_acme_challenge_arm' \
    'appliance_acme_challenge_read_request_line' \
    'appliance_acme_challenge_arm_token' \
    'appliance_acme_challenge_decide' \
    'appliance_acme_challenge_decide_line' \
    'appliance_acme_challenge_disarm' \
    'appliance_acme_challenge_status_string'; do
    grep -Fq -- "$item508_inc7_name" kernel/include/appliance_acme_challenge.h || {
        echo "ITEM-508 inc7: заголовок не публикует $item508_inc7_name" >&2
        exit 1
    }
done

# The one answerable path and the two answerable methods are the code's own.
grep -Fq 'APPLIANCE_ACME_CHALLENGE_PREFIX' \
    kernel/src/appliance_acme_challenge.c || {
    echo "ITEM-508 inc7: ответ не привязан к well-known префиксу" >&2
    exit 1
}
grep -Fq '"GET"' kernel/src/appliance_acme_challenge.c || {
    echo "ITEM-508 inc7: слушатель не знает GET" >&2
    exit 1
}
grep -Fq '"HEAD"' kernel/src/appliance_acme_challenge.c || {
    echo "ITEM-508 inc7: слушатель не знает HEAD" >&2
    exit 1
}
# The lifetime is enforced against the deadline, and arming refuses a clock
# whose lifetime would wrap it.
grep -Fq 'now_epoch_seconds > arm->deadline_epoch_seconds' \
    kernel/src/appliance_acme_challenge.c || {
    echo "ITEM-508 inc7: срок жизни токена не проверяется" >&2
    exit 1
}
grep -Fq 'UINT32_MAX - APPLIANCE_ACME_CHALLENGE_LIFETIME_SECONDS' \
    kernel/src/appliance_acme_challenge.c || {
    echo "ITEM-508 inc7: переполнение срока жизни не отвергается" >&2
    exit 1
}
# The token alphabet is the base64url one, so a token can never bring a path
# separator or a query into the answer.
grep -Fq "appliance_acme_challenge_token_valid" \
    kernel/src/appliance_acme_challenge.c || {
    echo "ITEM-508 inc7: алфавит токена не проверяется" >&2
    exit 1
}

# The module is decision-only: no allocation, no socket, no console, no TLS.
if grep -Eq 'malloc|calloc|realloc|fopen|socket|connect\(|listen\(' \
        kernel/src/appliance_acme_challenge.c; then
    echo "ITEM-508 inc7: решение выделяет память или ходит в сеть" >&2
    exit 1
fi
if grep -Eq 'serial_write|console_write|printf' \
        kernel/src/appliance_acme_challenge.c; then
    echo "ITEM-508 inc7: решение печатает само вместо записи" >&2
    exit 1
fi
if grep -Eq 'mbedtls|psa_' kernel/include/appliance_acme_challenge.h \
        kernel/src/appliance_acme_challenge.c; then
    echo "ITEM-508 inc7: решение зависит от TLS-профиля" >&2
    exit 1
fi

# The proof: the line, the exact path, the methods, the lifetime and disarm.
for item508_inc7_case in \
    'a well-formed request line parses' \
    'the parsed method and target are exact' \
    'a line ending in a bare LF and HTTP/1.0 parses' \
    'a truncated request line is refused' \
    'a null request line is refused' \
    'the malformed request line %zu is refused' \
    'the longest target fits exactly' \
    'a target one byte longer than the buffer is refused' \
    'a refused target is not partly copied' \
    'the listener answers the request line with the key authorization' \
    'the listener answers a HEAD line with no body' \
    'the listener refuses a line for another token' \
    'the listener refuses a line with a query string' \
    'the listener refuses a POST line' \
    'a malformed line never reaches the decision' \
    'a disarmed listener refuses the request line' \
    'a GET of the exact path is answered' \
    'the body is exactly the key authorization' \
    'HEAD answers the headers with no body' \
    'a trailing slash is not the exact path' \
    'a query string is not the exact path' \
    'a fragment is not the exact path' \
    'a lookalike prefix is not served' \
    'the token comparison does not fold case' \
    'the method comparison does not fold case' \
    'a token past its lifetime is expired' \
    'an unvalidated clock at the request is expired' \
    'an unvalidated clock refuses to arm' \
    'a clock whose lifetime would wrap the deadline refuses to arm' \
    'the replaced token'"'"'s path is not found' \
    'an unarmed listener answers no challenge' \
    'disarm clears the token, the answer and the count' \
    'a refused request is not counted as served'; do
    grep -Fq -- "$item508_inc7_case" tests/appliance-acme-challenge-host.c || {
        echo "ITEM-508 inc7: host-проба не проверяет: $item508_inc7_case" >&2
        exit 1
    }
done

# The build: the host proof exists and is in both aggregates.
grep -Fq 'test-appliance-acme-challenge-host' Makefile || {
    echo "ITEM-508 inc7: Makefile не содержит цель host-пробы" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile line is the literal checked
[[ $(grep -c '$(MAKE) test-appliance-acme-challenge-host' Makefile) -ge 2 ]] || {
    echo "ITEM-508 inc7: host-проба не в обоих наборах" >&2
    exit 1
}

# The documents: the increment is recorded where the challenge is read.
grep -Fq 'ITEM-508' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc7: ARCHITECTURE не записывает HTTP-01" >&2
    exit 1
}
grep -Fq 'increment 7' docs/ROADMAP.md || {
    echo "ITEM-508 inc7: ROADMAP не записывает инкремент" >&2
    exit 1
}
