#!/usr/bin/env bash
# ITEM-508 increment 3: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The ACME replies the client role reads, without a network and without a JSON
# library: one bounded reader and the objects RFC 8555 answers with.
#
# The item's evidence names the reply parsing explicitly - `newAccount`,
# `newOrder`, `challenge`, `finalize` on fixed data - and this increment is it.
# The claims pinned here:
#
#   * the reader is bounded and refusing, not a document model: it never
#     allocates, never copies beyond the caller's fields, treats escape
#     sequences it does not decode as refusals, refuses trailing bytes and
#     bounds its nesting;
#   * every field is validated as the thing it claims to be before it is
#     stored: a URL must be absolute https (the item's only outbound
#     connection is the server the reply names), a challenge token must be the
#     base64url alphabet (it is about to be printed on the wire), a status
#     must be one RFC 8555 defines for that object, and an order's identifiers
#     must be exact DNS names and are bounded;
#   * the authorization reader recognises `http-01` and refuses an
#     authorization that carries none - TLS-ALPN-01 stays impossible, ALPN is
#     forbidden by the profile - and it keeps the challenge's own status;
#   * the host proof drives all of it on fixed data, with no network and no
#     ACME server, and it is part of the host suite.
for item508_inc3_file in \
    kernel/include/appliance_acme_json.h \
    kernel/src/appliance_acme_json.c \
    kernel/include/appliance_acme_message.h \
    kernel/src/appliance_acme_message.c \
    tests/appliance-acme-message-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc3_file ]] || {
        echo "ITEM-508 inc3: отсутствует $item508_inc3_file" >&2
        exit 1
    }
done

# The reader: bounded depth, no allocation, no file or network surface.
for item508_inc3_reader in \
    'APPLIANCE_ACME_JSON_DEPTH_MAX' \
    'APPLIANCE_ACME_JSON_STRING' \
    'APPLIANCE_ACME_JSON_ARRAY'; do
    grep -Fq -- "$item508_inc3_reader" kernel/include/appliance_acme_json.h || {
        echo "ITEM-508 inc3: reader не публикует $item508_inc3_reader" >&2
        exit 1
    }
done
if grep -Eq 'malloc|calloc|realloc|free\(' \
        kernel/src/appliance_acme_json.c \
        kernel/src/appliance_acme_message.c; then
    echo "ITEM-508 inc3: разборщик выделяет память" >&2
    exit 1
fi
if grep -Eq 'getenv|fopen|socket|connect\(|serial_write' \
        kernel/src/appliance_acme_json.c \
        kernel/src/appliance_acme_message.c; then
    echo "ITEM-508 inc3: разборщик ходит в файлы, сеть или журнал" >&2
    exit 1
fi

# The messages: the validated shapes, the https rule and http-01.
for item508_inc3_message in \
    'APPLIANCE_ACME_MESSAGE_BAD_URL' \
    'APPLIANCE_ACME_MESSAGE_BAD_TOKEN' \
    'APPLIANCE_ACME_MESSAGE_BAD_STATUS' \
    'APPLIANCE_ACME_MESSAGE_BAD_IDENTIFIER' \
    'APPLIANCE_ACME_MESSAGE_TOO_MANY' \
    'APPLIANCE_ACME_MESSAGE_NO_HTTP01'; do
    grep -Fq -- "$item508_inc3_message" \
        kernel/include/appliance_acme_message.h || {
        echo "ITEM-508 inc3: сообщения не публикуют $item508_inc3_message" >&2
        exit 1
    }
done
grep -Fq '"https://"' kernel/src/appliance_acme_message.c || {
    echo "ITEM-508 inc3: URL не обязан быть абсолютным https" >&2
    exit 1
}
grep -Fq '"http-01"' kernel/src/appliance_acme_message.c || {
    echo "ITEM-508 inc3: разбор авторизации не знает http-01" >&2
    exit 1
}
for item508_inc3_parser in \
    appliance_acme_parse_directory \
    appliance_acme_parse_account \
    appliance_acme_parse_order \
    appliance_acme_parse_authorization; do
    grep -Fq -- "$item508_inc3_parser" kernel/include/appliance_acme_message.h || {
        echo "ITEM-508 inc3: заголовок не публикует $item508_inc3_parser" >&2
        exit 1
    }
done

# The build: both units are kernel objects and the proof is in the host suite.
for item508_inc3_source in \
    kernel/src/appliance_acme_json.c \
    kernel/src/appliance_acme_message.c; do
    grep -Fq -- "$item508_inc3_source" Makefile || {
        echo "ITEM-508 inc3: Makefile не собирает $item508_inc3_source" >&2
        exit 1
    }
done
grep -Fq 'test-appliance-acme-message-host' Makefile || {
    echo "ITEM-508 inc3: Makefile не содержит цель host-пробы" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile line is the literal checked
[[ $(grep -c '$(MAKE) test-appliance-acme-message-host' Makefile) -ge 2 ]] || {
    echo "ITEM-508 inc3: host-проба не в обоих наборах" >&2
    exit 1
}

# The proof: the item's named shapes are all driven.
for item508_inc3_case in \
    'the directory is parsed' \
    "an account's valid status is read" \
    "the order's status and counts are read" \
    'http-01 is found behind another challenge' \
    'the authorization and finalize URLs are read' \
    'a token that is not base64url is refused'; do
    grep -Fq -- "$item508_inc3_case" tests/appliance-acme-message-host.c || {
        echo "ITEM-508 inc3: host-проба не проверяет: $item508_inc3_case" >&2
        exit 1
    }
done

# The documents: the increment is recorded where the client role is read.
grep -Fq 'ITEM-508' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc3: ARCHITECTURE не записывает разбор ответов" >&2
    exit 1
}
grep -Fq 'increment 3' docs/ROADMAP.md || {
    echo "ITEM-508 inc3: ROADMAP не записывает инкремент" >&2
    exit 1
}
