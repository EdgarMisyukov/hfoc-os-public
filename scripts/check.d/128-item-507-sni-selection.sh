#!/usr/bin/env bash
# ITEM-507 increment 3: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# SNI selects one declared certificate per handshake, fail-closed.
#
# Increment 1 enabled SNI in the frozen profile and increment 2 made the
# name -> certificate/key table durable.  This increment is the decision
# itself: the pure rule the handshake applies, the callback that applies it,
# and the one bounded record per handshake that says which name the client
# presented, which certificate was chosen and why.
#
#   * the rule is `appliance_certificate_name_select`: an exact name (in any
#     case) is EXACT; a client without SNI is not an unknown name and gets the
#     declared identity; an unknown or malformed name is DEFAULT only when the
#     durable record declares the default certificate, and REFUSED otherwise;
#   * the callback consults the table parsed once at listener setup - it must
#     not allocate - and sets `mbedtls_ssl_set_hs_own_cert` for an exact name,
#     leaves the configured certificate for a default, and returns non-zero to
#     abort the handshake for a refusal;
#   * a store that is absent entirely is the pre-507 appliance and keeps
#     serving its one certificate; a record that exists but cannot be trusted
#     declares nothing, so unknown names are refused;
#   * one record per handshake (`[PRODUCTION-TLS-SNI] name=... result=...
#     cert=... reason=...`) is published through the ordinary-boot loop, and it
#     never carries certificate or key bytes; the SNI name the client chose is
#     sanitized before it reaches the journal;
#   * a handshake that presented no name says so explicitly at completion.
for item507_inc3_file in \
    kernel/include/appliance_certificate_name_store.h \
    kernel/src/appliance_certificate_name_store.c \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/src/kernel.c \
    tests/appliance-certificate-name-store-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item507_inc3_file ]] || {
        echo "ITEM-507 inc3: отсутствует $item507_inc3_file" >&2
        exit 1
    }
done

# The rule and its three outcomes are part of the model.
for item507_inc3_rule in \
    'APPLIANCE_CERTIFICATE_NAME_SELECTION_EXACT = 0' \
    'APPLIANCE_CERTIFICATE_NAME_SELECTION_DEFAULT = 1' \
    'APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED = 2' \
    'appliance_certificate_name_select('; do
    grep -Fq -- "$item507_inc3_rule" \
        kernel/include/appliance_certificate_name_store.h || {
        echo "ITEM-507 inc3: модель не объявляет правило выбора:" \
            "$item507_inc3_rule" >&2
        exit 1
    }
done
for item507_inc3_select in \
    'APPLIANCE_CERTIFICATE_NAME_SELECTION_EXACT' \
    'APPLIANCE_CERTIFICATE_NAME_SELECTION_DEFAULT' \
    'APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED' \
    'name == NULL || name_length == 0U' \
    'APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE'; do
    grep -Fq -- "$item507_inc3_select" \
        kernel/src/appliance_certificate_name_store.c || {
        echo "ITEM-507 inc3: правило выбора не реализовано:" \
            "$item507_inc3_select" >&2
        exit 1
    }
done

# The serving path: parsed pairs, the registered callback, the record and the
# no-name case.
for item507_inc3_serving in \
    'appliance_production_serving_sni_callback' \
    'appliance_production_serving_publish_name_table' \
    'appliance_production_serving_sni_record' \
    'appliance_production_serving_sni_sanitize' \
    'mbedtls_ssl_conf_sni(' \
    'mbedtls_ssl_set_hs_own_cert(' \
    'mbedtls_x509_crt_parse_der(' \
    'mbedtls_pk_parse_key(' \
    'APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_INVALID' \
    'connection->sni_present' \
    '"[PRODUCTION-TLS-SNI] name="' \
    '"REFUSED"' \
    'serving->name_usable[index]'; do
    grep -Fq -- "$item507_inc3_serving" \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-507 inc3: обслуживание не реализует:" \
            "$item507_inc3_serving" >&2
        exit 1
    }
done
# The callback must not be reached through the client profile: SNI stays a
# server-side presentation choice.
grep -Fq 'APPLIANCE_PRODUCTION_SERVING_SNI_RECORD_MAX 384U' \
    kernel/include/appliance_production_serving.h || {
    echo "ITEM-507 inc3: запись выбора не ограничена" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_take_sni_record(' \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-507 inc3: запись выбора не отдаётся циклу" >&2
    exit 1
}
grep -Fq 'static void log_production_sni(void)' kernel/src/kernel.c || {
    echo "ITEM-507 inc3: цикл не публикует запись выбора" >&2
    exit 1
}
grep -Fq 'log_production_sni();' kernel/src/kernel.c || {
    echo "ITEM-507 inc3: цикл не вызывает публикацию записи выбора" >&2
    exit 1
}
# The QEMU proof requires the record on a real handshake, in the boot that has
# no committed name record.
grep -Fq \
    'PRODUCTION-TLS-SNI] name=hfoc-appliance result=DEFAULT cert=default' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-507 inc3: QEMU-проба не требует записи выбора" >&2
    exit 1
}

# The proof: the selection cases and the no-name rule are asserted.
for item507_inc3_proof in \
    'an exact name selects its own certificate' \
    'an exact name selects in any case' \
    'an unknown name is refused, not given another identity' \
    'a malformed name is refused' \
    'a client without SNI gets the declared identity' \
    'a declared default is what an unknown name gets' \
    'a malformed name follows the same declaration' \
    'a declared default never shadows an exact name' \
    'a missing table refuses'; do
    grep -Fq -- "$item507_inc3_proof" \
        tests/appliance-certificate-name-store-host.c || {
        echo "ITEM-507 inc3: проба не утверждает:" \
            "$item507_inc3_proof" >&2
        exit 1
    }
done

# The documents record the increment.
grep -Fq 'result=REFUSED' docs/ARCHITECTURE.md || {
    echo "ITEM-507 inc3: ARCHITECTURE не описывает выбор" >&2
    exit 1
}
grep -Fq 'ITEM-507 increment 3' docs/ROADMAP.md || {
    echo "ITEM-507 inc3: ROADMAP не записывает инкремент" >&2
    exit 1
}
