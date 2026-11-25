#!/usr/bin/env bash
# ITEM-508 increment 12: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The forbidden-symbol check can except one symbol for one named source.
#
# The ACME client binding is the only file that may ever call the client
# handshake symbol, and the verifier forbids that symbol in every source it is
# given.  Instead of repealing the prohibition, this increment gives the
# verifier a checked exception: `--except=<symbol>:<source>` skips exactly that
# pair, and the exception must name a source the invocation checks and a symbol
# the verifier forbids.  This slice pins the mechanism, its refusals and the
# host proof of every case.
for item508_inc12_file in \
    scripts/verify-mbedtls-config.sh \
    tests/test-mbedtls-config-host.sh \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc12_file ]] || {
        echo "ITEM-508 inc12: отсутствует $item508_inc12_file" >&2
        exit 1
    }
done

# The argument, the pair matching and every refusal by name.
# shellcheck disable=SC2016  # the pins are literal source, not expansions
for item508_inc12_pin in \
    '--except=' \
    'malformed exception (expected symbol:source):' \
    'exception names a missing source:' \
    'exception names a source that is not checked:' \
    'exception names a symbol that is not forbidden:' \
    '${EXCEPT_SYMBOLS[$index]} == "$symbol"' \
    '${EXCEPT_SOURCES[$index]} == "$source"'; do
    grep -Fq -- "$item508_inc12_pin" scripts/verify-mbedtls-config.sh || {
        echo "ITEM-508 inc12: верификатор не пинит «$item508_inc12_pin»" >&2
        exit 1
    }
done
# The exception is not a repeal: the client symbol stays forbidden.
grep -Fq 'mbedtls_ssl_handshake_client_step' \
    scripts/verify-mbedtls-config.sh || {
    echo "ITEM-508 inc12: клиентский символ пропал из запретов" >&2
    exit 1
}

# The host proof drives every case: the pair passes, the same symbol in an
# unnamed source fails, another symbol in the excepted source fails, and every
# dangling exception fails.
for item508_inc12_case in \
    'the excepted source passes' \
    'client symbol in a source without its own exception' \
    'another forbidden symbol in the excepted source' \
    'exception for a source that is not checked' \
    'exception for a source that does not exist' \
    'exception for a symbol that is not forbidden' \
    'a malformed exception'; do
    grep -Fq -- "$item508_inc12_case" tests/test-mbedtls-config-host.sh || {
        echo "ITEM-508 inc12: проба не проверяет «$item508_inc12_case»" >&2
        exit 1
    }
done

# The documents name the increment and its bound.
for item508_inc12_doc in docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Fq 'ITEM-508 increment 12' "$item508_inc12_doc" || {
        echo "ITEM-508 inc12: $item508_inc12_doc не называет инкремент" >&2
        exit 1
    }
done
