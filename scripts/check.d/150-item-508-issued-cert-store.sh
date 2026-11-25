#!/usr/bin/env bash
# ITEM-508 increment 23: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The chain ACME issued is persisted with its own provenance.
#
# The appliance can already obtain and verify a chain (increments 17-21), but
# nothing wrote it down: the record the TLS listener serves from knows only
# "generated here" and "imported by an operator".  This increment adds the
# third provenance to the same v2 record - `SOURCE_ACME` - through a sibling of
# the operator writer, so a verified chain survives the next boot and the
# serving record says what it actually is.  An unknown provenance on disk is a
# corrupt record, not a silent "generated".  This slice pins the constant, the
# writer, the read-back, the serving wording, the host proof and the documents.
for item508_inc23_file in \
    kernel/include/appliance_production_certificate.h \
    kernel/src/appliance_production_certificate.c \
    kernel/src/appliance_production_serving.c \
    tests/appliance-certificate-upload-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc23_file ]] || {
        echo "ITEM-508 inc23: отсутствует $item508_inc23_file" >&2
        exit 1
    }
done

# 1. The header declares the third provenance, the persistent view and the
#    writer; the operator contract is untouched.
for item508_inc23_header_pin in \
    '#define APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_ACME 2U' \
    'bool acme_issued;' \
    'uint32_t acme_count;' \
    'appliance_production_certificate_adopt_acme(' \
    '#define APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_OPERATOR 1U'; do
    grep -Fq -- "$item508_inc23_header_pin" \
        kernel/include/appliance_production_certificate.h || {
        echo "ITEM-508 inc23: заголовок не содержит «$item508_inc23_header_pin»" \
            >&2
        exit 1
    }
done

# 2. One writer, two provenances: the shared body, both wrappers, the ACME
#    branch in the header writer and the counter.
for item508_inc23_source_pin in \
    'appliance_production_certificate_adopt_source(' \
    'APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_ACME' \
    'source != APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_GENERATED' \
    'source != APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_OPERATOR' \
    'source != APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_ACME' \
    'certificate->acme_issued' \
    'certificate->imported' \
    '++certificate->acme_count;' \
    '++certificate->import_count;'; do
    grep -Fq -- "$item508_inc23_source_pin" \
        kernel/src/appliance_production_certificate.c || {
        echo "ITEM-508 inc23: запись не содержит «$item508_inc23_source_pin»" \
            >&2
        exit 1
    }
done
# The wrappers go through the one body, so the operator path keeps the exact
# bounds and read-back it was proven with.
item508_inc23_adopt_calls=$(
    grep -c 'appliance_production_certificate_adopt_source(' \
        kernel/src/appliance_production_certificate.c
)
[[ $item508_inc23_adopt_calls -eq 3 ]] || {
    echo "ITEM-508 inc23: у записи не один общий писатель" >&2
    exit 1
}

# 3. The serving record names the issued chain; the operator and generated
#    wordings stay.
grep -Fq 'persisted_acme_issued' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-508 inc23: serving не называет выданную цепочку" >&2
    exit 1
}
for item508_inc23_serving_kept in \
    'persisted_operator' \
    'persisted_self_signed' \
    'ephemeral_self_signed'; do
    grep -Fq -- "$item508_inc23_serving_kept" \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-508 inc23: serving потерял «$item508_inc23_serving_kept»" >&2
        exit 1
    }
done

# 4. The host proof drives the reboot, the provenance and the foreign source.
for item508_inc23_proof_case in \
    'appliance_production_certificate_adopt_acme(' \
    'the verified ACME chain is written as record v2' \
    'the owner counts the ACME record, not an operator import' \
    'the loaded record keeps the ACME provenance' \
    'the loaded ACME record serves the issued certificate byte for byte' \
    'the loaded ACME record carries its issuing chain byte for byte' \
    'the loaded ACME record carries the key the CSR named' \
    'the record names the ACME provenance on disk' \
    'a foreign provenance is refused and regenerated, never relabelled'; do
    grep -Fq -- "$item508_inc23_proof_case" \
        tests/appliance-certificate-upload-host.c || {
        echo "ITEM-508 inc23: проба не проверяет «$item508_inc23_proof_case»" \
            >&2
        exit 1
    }
done

# 5. The documents name the increment and its boundary.
grep -Fq 'ITEM-508 increment 23' docs/ROADMAP.md || {
    echo "ITEM-508 inc23: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 23' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc23: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
