#!/usr/bin/env bash
# ITEM-507 increment 2: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The name -> certificate/key table is durable and fail-closed.
#
# ITEM-507 serves a different certificate per exact DNS name; increment 1 made
# SNI possible in the frozen profile, this increment owns the declaration it
# selects from.  The claims pinned here:
#
#   * the model holds at most eight exact DNS names (the route table maximum),
#     each with certificate DER and private-key DER within the ITEM-503 bounds,
#     names normalized by the route table's own public rule so a name a route
#     carries and a name a certificate is declared for are the same name;
#   * duplicates (in any case) are refused, an unknown name is NOT_FOUND and a
#     wildcard is BAD_NAME, so the selection can name why it refused;
#   * an unknown SNI name is refused unless the record explicitly declares the
#     default certificate, and that declaration is part of the durable record;
#   * the record lives after the twelve blocks of the ITEM-503 certificate
#     record in the same sixty-four-block CERTIFICATE-namespace store, so no
#     layout, capacity or installer contract changes;
#   * a missing, corrupt, checksum-failing or foreign-version record is the
#     empty table, never a partial one, and a sealed volume is never written in
#     the clear; save reads the record back before it reports OK;
#   * the key is never rendered: the model's rendering names the certificate's
#     byte count and the presence of the key only, and a removal zeroizes the
#     slot.
for item507_inc2_file in \
    kernel/include/appliance_certificate_name_store.h \
    kernel/src/appliance_certificate_name_store.c \
    tests/appliance-certificate-name-store-host.c \
    scripts/check.d/127-item-507-certificate-name-store.sh \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item507_inc2_file ]] || {
        echo "ITEM-507 inc2: отсутствует $item507_inc2_file" >&2
        exit 1
    }
done

# The bounds and the placement: eight names of the ITEM-503 DER bounds, and the
# record starts after the ITEM-503 certificate record in the shared store.
for item507_inc2_bound in \
    '#define APPLIANCE_CERTIFICATE_NAME_MAX_COUNT 8U' \
    '#define APPLIANCE_CERTIFICATE_NAME_MAX 253U' \
    '#define APPLIANCE_CERTIFICATE_NAME_DER_MAX' \
    '#define APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX' \
    '#define APPLIANCE_CERTIFICATE_NAME_STORE_BLOCK_OFFSET' \
    'APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BLOCKS' \
    '#define APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BLOCKS 32U' \
    '#define APPLIANCE_CERTIFICATE_NAME_STORE_MAGIC UINT32_C(0x54534E43)'; do
    grep -Fq -- "$item507_inc2_bound" \
        kernel/include/appliance_certificate_name_store.h || {
        echo "ITEM-507 inc2: граница записи не найдена:" \
            "$item507_inc2_bound" >&2
        exit 1
    }
done

# The model: the route table's own normalizer, the fail-closed lookup and the
# explicit default declaration.
for item507_inc2_model in \
    'appliance_certificate_name_table_add(' \
    'appliance_certificate_name_table_remove(' \
    'appliance_certificate_name_table_lookup(' \
    'appliance_certificate_name_table_validate(' \
    'appliance_certificate_name_table_set_default(' \
    'appliance_certificate_name_render(' \
    'APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE = 0' \
    'APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE = 1' \
    'APPLIANCE_CERTIFICATE_NAME_DUPLICATE_NAME' \
    'APPLIANCE_CERTIFICATE_NAME_NOT_FOUND' \
    'APPLIANCE_CERTIFICATE_NAME_BAD_NAME'; do
    grep -Fq -- "$item507_inc2_model" \
        kernel/include/appliance_certificate_name_store.h || {
        echo "ITEM-507 inc2: модель не объявляет:" \
            "$item507_inc2_model" >&2
        exit 1
    }
done
grep -Fq 'appliance_route_name_normalize(' \
    kernel/src/appliance_certificate_name_store.c || {
    echo "ITEM-507 inc2: имя нормализуется не правилом таблицы маршрутов" >&2
    exit 1
}
grep -Fq 'appliance_certificate_name_store_zero(' \
    kernel/src/appliance_certificate_name_store.c || {
    echo "ITEM-507 inc2: удалённая запись не обнуляется" >&2
    exit 1
}

# The store: bound as a sub-range of the certificate store entry, with the
# sealed refusal, the read-back and the fail-closed source.
for item507_inc2_store in \
    'APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE' \
    'APPLIANCE_STATE_STORE_ID_CERTIFICATE' \
    'appliance_state_block_device_bind_range(' \
    'appliance_certificate_name_store_bind(' \
    'APPLIANCE_CERTIFICATE_NAME_STORE_STORE_SEALED' \
    'APPLIANCE_CERTIFICATE_NAME_STORE_WRITE_FAILED' \
    'APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CHECKSUM' \
    'APPLIANCE_CERTIFICATE_NAME_STORE_TABLE_REFUSED' \
    'SOURCE_DEFAULT_MISSING' \
    'SOURCE_DEFAULT_INVALID' \
    'SOURCE_DEFAULT_UNAVAILABLE'; do
    grep -Fq -- "$item507_inc2_store" \
        kernel/src/appliance_certificate_name_store.c \
        kernel/include/appliance_certificate_name_store.h || {
        echo "ITEM-507 inc2: хранилище не реализует:" \
            "$item507_inc2_store" >&2
        exit 1
    }
done

# The host proof and its registration: the target runs it, the host suite
# carries it, and the test pins the cases this slice claims.
grep -Fq 'test-appliance-certificate-name-store-host' Makefile || {
    echo "ITEM-507 inc2: Makefile не содержит пробу хранилища имён" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile recipe text is pinned literally
grep -Fq '	$(MAKE) test-appliance-certificate-name-store-host' Makefile || {
    echo "ITEM-507 inc2: host-suite не содержит пробу хранилища имён" >&2
    exit 1
}
for item507_inc2_proof in \
    'a 253-byte name is accepted' \
    'a wildcard is not a name this table can mean' \
    'an unknown name is NOT_FOUND, never the first entry' \
    'the certificate and the key survive the round trip byte for byte' \
    'the lookup stays case-insensitive after the round trip' \
    'an explicitly empty table is committed, not missing' \
    'a body that disagrees with its checksum is refused whole' \
    'a length that overruns the record is refused before it is read' \
    'a read-back that disagrees fails the save' \
    'a deleted entry leaves no key material behind' \
    'the rendering carries no payload bytes' \
    'no state volume means no stored names, honestly reported' \
    'the name record starts after the ITEM-503 record, in the same store'; do
    grep -Fq -- "$item507_inc2_proof" \
        tests/appliance-certificate-name-store-host.c || {
        echo "ITEM-507 inc2: проба не утверждает:" \
            "$item507_inc2_proof" >&2
        exit 1
    }
done

# The documents: the live architecture names the new store, and the roadmap
# records the increment next to increment 1.
grep -Fq 'appliance_certificate_name_store' docs/ARCHITECTURE.md || {
    echo "ITEM-507 inc2: ARCHITECTURE не описывает хранилище имён" >&2
    exit 1
}
grep -Fq 'ITEM-507 increment 2' docs/ROADMAP.md || {
    echo "ITEM-507 inc2: ROADMAP не записывает инкремент" >&2
    exit 1
}
