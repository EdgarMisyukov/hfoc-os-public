#!/usr/bin/env bash
# ITEM-504: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The facade's route table is a pure value: name -> literal backend, with the
# backend port, whether the hop is TLS, and where the route came from.  The
# first increment of ITEM-504 lands that value and its proof, so the later
# mutation plane and panel section have one owner of the grammar:
#
#   * a name is a DNS name (1..253 bytes, labels 1..63 from [a-z0-9-], no
#     leading or trailing hyphen), normalized to lower case; one trailing dot
#     is accepted and removed, a wildcard is not a name;
#   * an HTTP `Host` value may carry an optional `:port`; an IPv6 literal is
#     refused as a name this table cannot carry;
#   * a backend is a literal IPv4 address that names one host: the unspecified
#     address, the limited broadcast, loopback and multicast are refused,
#     exactly as the ITEM-449 forward declaration refuses them;
#   * duplicate names (in any case) and a ninth route are refused; nothing is
#     stored on a refusal;
#   * a lookup is fail-closed: an unknown name is NOT_FOUND, never the first
#     route that happens to exist;
#   * rendering never truncates: a buffer that cannot hold the terminator is
#     refused with BUFFER_TOO_SMALL.

for item504_file in \
    kernel/include/appliance_route_table.h \
    kernel/src/appliance_route_table.c \
    tests/appliance-route-table-host.c; do
    test -f "$item504_file" || {
        echo "ITEM-504: отсутствует файл: $item504_file" >&2
        exit 1
    }
done

# ------------------------------------------------------------ the model ------
for item504_api in \
    APPLIANCE_ROUTE_TABLE_VERSION \
    APPLIANCE_ROUTE_TABLE_MAX \
    APPLIANCE_ROUTE_NAME_MAX \
    APPLIANCE_ROUTE_BACKEND_ADDRESS_LENGTH \
    APPLIANCE_ROUTE_RECORD_MAX \
    appliance_route_table_init \
    appliance_route_table_add \
    appliance_route_table_remove \
    appliance_route_table_lookup \
    appliance_route_table_validate \
    appliance_route_render \
    appliance_route_table_render \
    appliance_route_name_normalize \
    appliance_route_name_from_host \
    appliance_route_source_string \
    appliance_route_status_string; do
    grep -Fq "$item504_api" kernel/include/appliance_route_table.h || {
        echo "ITEM-504: модель маршрутов не публикует: $item504_api" >&2
        exit 1
    }
done
for item504_refusal in \
    APPLIANCE_ROUTE_BAD_NAME \
    APPLIANCE_ROUTE_NAME_TOO_LONG \
    APPLIANCE_ROUTE_BAD_BACKEND \
    APPLIANCE_ROUTE_BACKEND_NOT_HOST \
    APPLIANCE_ROUTE_BAD_PORT \
    APPLIANCE_ROUTE_BAD_SOURCE \
    APPLIANCE_ROUTE_DUPLICATE_NAME \
    APPLIANCE_ROUTE_TABLE_FULL \
    APPLIANCE_ROUTE_NOT_FOUND \
    APPLIANCE_ROUTE_BUFFER_TOO_SMALL \
    APPLIANCE_ROUTE_BAD_VERSION; do
    grep -Fq "$item504_refusal" kernel/include/appliance_route_table.h || {
        echo "ITEM-504: отказ не объявлен: $item504_refusal" >&2
        exit 1
    }
    grep -Fq "$item504_refusal" kernel/src/appliance_route_table.c || {
        echo "ITEM-504: отказ не назван в модели: $item504_refusal" >&2
        exit 1
    }
done
grep -Fq 'return APPLIANCE_ROUTE_NOT_FOUND;' \
    kernel/src/appliance_route_table.c || {
    echo "ITEM-504: неизвестное имя не отвергается fail-closed" >&2
    exit 1
}
grep -Fq 'route_to_lower' kernel/src/appliance_route_table.c || {
    echo "ITEM-504: имя не нормализуется к нижнему регистру" >&2
    exit 1
}
grep -Fq 'route_backend_is_host' kernel/src/appliance_route_table.c || {
    echo "ITEM-504: backend не проверяется как хост" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ROUTE_BACKEND_ADDRESS_LENGTH' \
    kernel/src/appliance_route_table.c || {
    echo "ITEM-504: backend не ограничен IPv4" >&2
    exit 1
}
grep -Fq "'-'" kernel/src/appliance_route_table.c || {
    echo "ITEM-504: дефис не назван в грамматике имени" >&2
    exit 1
}
grep -Fq 'label_length > 63U' kernel/src/appliance_route_table.c || {
    echo "ITEM-504: метка имени не ограничена 63 байтами" >&2
    exit 1
}
grep -Fq 'host[0] == '"'"'['"'"'' kernel/src/appliance_route_table.c || {
    echo "ITEM-504: литерал IPv6 не отвергается как имя" >&2
    exit 1
}
grep -Fq 'tls_to_backend' kernel/include/appliance_route_table.h || {
    echo "ITEM-504: признак TLS до backend не объявлен" >&2
    exit 1
}

# ------------------------------------------------------------- the proofs ----
grep -Fq 'test-appliance-route-table-host' Makefile || {
    echo "ITEM-504: Makefile не содержит пробы модели маршрутов" >&2
    exit 1
}
# shellcheck disable=SC2016  # the literal Makefile text is the contract
grep -Fq '$(MAKE) test-appliance-route-table-host' Makefile || {
    echo "ITEM-504: host-набор не запускает пробу модели маршрутов" >&2
    exit 1
}
for item504_proof in \
    'an empty table refuses every name as not found' \
    'a well-formed panel route is added' \
    'a duplicate name in another case is refused' \
    'a refused duplicate does not change the table' \
    'a 64-byte label is refused' \
    'a name over 253 bytes is refused as too long' \
    'the Host value normalizes to the declared name' \
    'the backend %s is decided as declared' \
    'one route over the limit is refused' \
    'a route whose stored name is not normalized is refused' \
    'a zero backend port is refused' \
    'the route record names the name, the backend, TLS and the source' \
    'a buffer without room for the terminator is refused, not truncated' \
    'the table rendering carries every route in table order' \
    'the longest name is exactly 253 bytes' \
    'the longest record is longer than the short bound yet still fits' \
    'the longest table rendering is inside the declared bound' \
    'an unknown name is refused as not found, never matched to the first route'; do
    grep -Fq "$item504_proof" tests/appliance-route-table-host.c || {
        echo "ITEM-504: host-проба не утверждает: $item504_proof" >&2
        exit 1
    }
done

# -------------------------------------------------------------- documents ----
grep -Fq 'ITEM-504' docs/ROADMAP.md || {
    echo "ITEM-504: ROADMAP не называет пункт" >&2
    exit 1
}
grep -Fq 'appliance_route_table' docs/ARCHITECTURE.md || {
    echo "ITEM-504: ARCHITECTURE не описывает модель маршрутов" >&2
    exit 1
}
grep -Fq 'ITEM-504' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-504: очередь не называет пункт" >&2
    exit 1
}
