#!/usr/bin/env bash
# ITEM-508 increment 22: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The finalize carries a real PKCS#10 written from the account key.
#
# Until this increment the flow posted the literal `{}` to the finalize URL
# (`appliance_acme_flow.c`), which no real CA can accept: RFC 8555 section 7.4
# wants a base64url PKCS#10.  The account key's private context is module-static
# in `appliance_acme_jws.c` and has no accessor, so the CSR is written inside
# that module and the caller receives only the bounded DER.  The frozen profile
# moves its X.509 write side - until now a delta of the serving header - into
# `hfoc_mbedtls_config.h` as a positive contract (version 4), while CSR and CRL
# parsing, PEM and Base64 stay refused; the offline stub checks the CSR it
# receives so a regression to `{}` fails the QEMU proof by name.  This slice
# pins all of that, the refusals, the record and the documents.
for item508_inc22_file in \
    kernel/include/hfoc_mbedtls_config.h \
    kernel/include/hfoc_mbedtls_tls_config.h \
    scripts/verify-mbedtls-config.sh \
    scripts/check.d/01-lwip-source-license-lock.sh \
    kernel/include/appliance_acme_jws.h \
    kernel/src/appliance_acme_jws.c \
    kernel/include/appliance_acme_flow.h \
    kernel/src/appliance_acme_flow.c \
    kernel/src/appliance_acme_order_qemu.c \
    scripts/hfoc-acme-directory-stub.py \
    scripts/test-acme-directory-stub-host.sh \
    scripts/test-qemu-acme-order.sh \
    tests/appliance-acme-jws-host.c \
    tests/appliance-acme-flow-host.c \
    tests/test-mbedtls-config-host.sh \
    tests/hfoc-mbedtls-manifest-host-config.h \
    tests/hfoc-tls-qemu-config.h \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc22_file ]] || {
        echo "ITEM-508 inc22: отсутствует $item508_inc22_file" >&2
        exit 1
    }
done

# 1. The profile: version 4, the write side required in one place, and the
#    serving header no longer redefining it.
grep -Fq '#define HFOC_MBEDTLS_CONFIG_VERSION 5' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "ITEM-508 inc22: живой профиль не на версии 4" >&2
    exit 1
}
for item508_inc22_define in \
    '#define MBEDTLS_X509_CREATE_C' \
    '#define MBEDTLS_X509_CRT_WRITE_C' \
    '#define MBEDTLS_X509_CSR_WRITE_C' \
    '#define MBEDTLS_PK_WRITE_C'; do
    grep -Fq -- "$item508_inc22_define" \
        kernel/include/hfoc_mbedtls_config.h || {
        echo "ITEM-508 inc22: профиль не определяет $item508_inc22_define" >&2
        exit 1
    }
done
if grep -Fq '#undef MBEDTLS_X509_CSR_WRITE_C' \
        kernel/include/hfoc_mbedtls_config.h; then
    echo "ITEM-508 inc22: профиль всё ещё снимает запись CSR" >&2
    exit 1
fi
for item508_inc22_parse_undef in \
    '#undef MBEDTLS_X509_CRL_PARSE_C' \
    '#undef MBEDTLS_X509_CSR_PARSE_C'; do
    grep -Fq -- "$item508_inc22_parse_undef" \
        kernel/include/hfoc_mbedtls_config.h || {
        echo "ITEM-508 inc22: разбор CSR/CRL больше не запрещён" >&2
        exit 1
    }
done
if grep -Fq '#define MBEDTLS_X509_CSR_WRITE_C' \
        kernel/include/hfoc_mbedtls_tls_config.h; then
    echo "ITEM-508 inc22: запись CSR всё ещё продублирована в serving-профиле" \
        >&2
    exit 1
fi

# The verifier carries the positive contract and still refuses the parsers.
for item508_inc22_verifier in \
    '#if HFOC_MBEDTLS_CONFIG_VERSION != 5' \
    '#error MBEDTLS_X509_CSR_WRITE_C is required for the ACME finalize CSR' \
    '#error MBEDTLS_X509_CREATE_C is required for the X.509 write side' \
    '#error X.509 CRL/CSR parse paths are forbidden'; do
    grep -Fq -- "$item508_inc22_verifier" \
        scripts/verify-mbedtls-config.sh || {
        echo "ITEM-508 inc22: верификатор не пинит «$item508_inc22_verifier»" \
            >&2
        exit 1
    }
done
grep -Fq '#define HFOC_MBEDTLS_CONFIG_VERSION 5' \
    scripts/check.d/01-lwip-source-license-lock.sh || {
    echo "ITEM-508 inc22: срез 01 не пинит живую версию 4" >&2
    exit 1
}
for item508_inc22_profile_case in \
    'disabled mbedTLS CSR writer' \
    'enabled mbedTLS CSR parsing' \
    'enabled mbedTLS CRL parsing'; do
    grep -Fq -- "$item508_inc22_profile_case" \
        tests/test-mbedtls-config-host.sh || {
        echo "ITEM-508 inc22: проба профиля не проверяет «$item508_inc22_profile_case»" \
            >&2
        exit 1
    }
done

# 2. The CSR is written inside the module that owns the private context, from
#    the account key, and the header exposes no key accessor.
for item508_inc22_jws_pin in \
    'appliance_acme_key_write_csr(' \
    'mbedtls_x509write_csr_set_subject_name(' \
    'mbedtls_x509write_csr_der(' \
    'appliance_acme_csr_subject_valid(' \
    'memmove(der, &der[capacity - (size_t)result], (size_t)result)' \
    'APPLIANCE_ACME_JWS_CSR_FAILED' \
    'the subject of the CSR is one common name'; do
    grep -Fq -- "$item508_inc22_jws_pin" \
        kernel/src/appliance_acme_jws.c || {
        echo "ITEM-508 inc22: JWS-модуль не содержит «$item508_inc22_jws_pin»" \
            >&2
        exit 1
    }
done
for item508_inc22_jws_bound in \
    '#define APPLIANCE_ACME_CSR_MAX 512U' \
    '#define APPLIANCE_ACME_CSR_CN_MAX 64U'; do
    grep -Fq -- "$item508_inc22_jws_bound" \
        kernel/include/appliance_acme_jws.h || {
        echo "ITEM-508 inc22: граница «$item508_inc22_jws_bound» не названа" \
            >&2
        exit 1
    }
done
grep -Fq 'appliance_acme_key_write_csr(' \
    kernel/include/appliance_acme_jws.h || {
    echo "ITEM-508 inc22: писатель CSR не объявлен" >&2
    exit 1
}
if grep -Fq 'key_context' kernel/include/appliance_acme_jws.h; then
    echo "ITEM-508 inc22: заголовок выдаёт приватный контекст ключа" >&2
    exit 1
fi

# 3. The flow builds the RFC 8555 payload and refuses a missing or oversized
#    CSR by name instead of sending anything.
for item508_inc22_flow_pin in \
    'const unsigned char *csr,' \
    'APPLIANCE_ACME_FLOW_CSR_REQUIRED' \
    'APPLIANCE_ACME_FLOW_CSR_TOO_LARGE' \
    'payload_prefix[] = "{\"csr\":\""' \
    'appliance_acme_base64url_encode(' \
    'false, payload, payload_length, f_rng,'; do
    grep -Fq -- "$item508_inc22_flow_pin" \
        kernel/src/appliance_acme_flow.c || {
        echo "ITEM-508 inc22: поток не содержит «$item508_inc22_flow_pin»" >&2
        exit 1
    }
done
for item508_inc22_flow_status in \
    'APPLIANCE_ACME_FLOW_CSR_REQUIRED = 13' \
    'APPLIANCE_ACME_FLOW_CSR_TOO_LARGE = 14'; do
    grep -Fq -- "$item508_inc22_flow_status" \
        kernel/include/appliance_acme_flow.h || {
        echo "ITEM-508 inc22: статус «$item508_inc22_flow_status» не назван" \
            >&2
        exit 1
    }
done
grep -Fq 'case APPLIANCE_ACME_FLOW_CSR_REQUIRED:' \
    kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc22: статус отказа CSR не печатается" >&2
    exit 1
}

# 4. The driver writes the CSR from the declared order name, records it before
#    the request, and hands it to the flow.
for item508_inc22_driver_pin in \
    'appliance_acme_key_write_csr(' \
    'acme_order_csr,' \
    'acme_order_publish_csr(acme_order_csr_length)' \
    '"[ACME-ORDER] csr bytes="' \
    '" subject="' \
    'acme_order_csr, acme_order_csr_length,'; do
    grep -Fq -- "$item508_inc22_driver_pin" \
        kernel/src/appliance_acme_order_qemu.c || {
        echo "ITEM-508 inc22: драйвер не содержит «$item508_inc22_driver_pin»" \
            >&2
        exit 1
    }
done
item508_inc22_csr_line=$(
    grep -n 'acme_order_publish_csr(acme_order_csr_length)' \
        kernel/src/appliance_acme_order_qemu.c | cut -d: -f1
)
item508_inc22_finalize_line=$(
    grep -n 'appliance_acme_flow_begin_finalize(' \
        kernel/src/appliance_acme_order_qemu.c | cut -d: -f1
)
[[ -n $item508_inc22_csr_line && -n $item508_inc22_finalize_line &&
   $item508_inc22_csr_line -lt $item508_inc22_finalize_line ]] || {
    echo "ITEM-508 inc22: запись CSR не предшествует запросу finalize" >&2
    exit 1
}

# 5. The stub checks what RFC 8555 requires of the finalize and refuses by
#    name; the host proof sends both a real CSR and a body without one.
for item508_inc22_stub_pin in \
    'def finalize_csr_refusal(' \
    'ORDER_IDENTIFIER = "hfoc-appliance"' \
    '"the finalize payload carries no csr"' \
    '"the csr signature does not verify"' \
    '"openssl"' \
    '"req"' \
    '"-verify"' \
    'ACME-STUB refusal:'; do
    grep -Fq -- "$item508_inc22_stub_pin" \
        scripts/hfoc-acme-directory-stub.py || {
        echo "ITEM-508 inc22: заглушка не содержит «$item508_inc22_stub_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'openssl req -new -key' scripts/test-acme-directory-stub-host.sh || {
    echo "ITEM-508 inc22: host-проба заглушки не строит настоящий CSR" >&2
    exit 1
}
grep -Fq 'r_finalize_no_csr' scripts/test-acme-directory-stub-host.sh || {
    echo "ITEM-508 inc22: host-проба заглушки не проверяет отказ без CSR" >&2
    exit 1
}
grep -Fq '\[ACME-ORDER\] csr bytes=' scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc22: прогон QEMU не требует записи CSR" >&2
    exit 1
}
grep -Fq 'ACME-STUB refusal:' scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc22: прогон QEMU не ловит отказ заглушки" >&2
    exit 1
}

# 6. The host proofs drive the writer, its refusals and the payload, and the
#    build compiles the write-side sources for them.
for item508_inc22_jws_case in \
    'the CSR is a bounded DER SEQUENCE' \
    'an unbound key cannot write a CSR' \
    'a subject that could rewrite the name is refused' \
    'a destination below the bound is refused before writing'; do
    grep -Fq -- "$item508_inc22_jws_case" \
        tests/appliance-acme-jws-host.c || {
        echo "ITEM-508 inc22: JWS-проба не проверяет «$item508_inc22_jws_case»" \
            >&2
        exit 1
    }
done
for item508_inc22_flow_case in \
    'a finalize without a CSR refuses and sends nothing' \
    'an oversized CSR refuses by name' \
    'the finalize payload is the base64url PKCS#10'; do
    grep -Fq -- "$item508_inc22_flow_case" \
        tests/appliance-acme-flow-host.c || {
        echo "ITEM-508 inc22: проба потока не проверяет «$item508_inc22_flow_case»" \
            >&2
        exit 1
    }
done
grep -Fq 'x509write_csr.c' Makefile || {
    echo "ITEM-508 inc22: сборка проб не компилирует писатель CSR" >&2
    exit 1
}
grep -Fq 'x509_create.c' Makefile || {
    echo "ITEM-508 inc22: сборка проб не компилирует X.509 create" >&2
    exit 1
}
grep -Fq 'build/test-appliance-acme-jws-host --write-csr' Makefile || {
    echo "ITEM-508 inc22: проба не отдаёт CSR независимой проверке openssl" >&2
    exit 1
}
grep -Fq '#define MBEDTLS_X509_CSR_WRITE_C' \
    tests/hfoc-mbedtls-manifest-host-config.h || {
    echo "ITEM-508 inc22: host-профиль не включает писатель CSR" >&2
    exit 1
}
# The item-270 TLS QEMU fixture never writes a certificate or a request and
# links no platform shim, so it turns the write side back off explicitly; a
# fixture that silently inherited the writer failed to link (CI run
# 37349182421, job acceptance-qemu, undefined strncpy from x509write_crt.c).
for item508_inc22_fixture_undef in \
    '#undef MBEDTLS_X509_CREATE_C' \
    '#undef MBEDTLS_X509_CRT_WRITE_C' \
    '#undef MBEDTLS_X509_CSR_WRITE_C' \
    '#undef MBEDTLS_PK_WRITE_C'; do
    grep -Fq -- "$item508_inc22_fixture_undef" \
        tests/hfoc-tls-qemu-config.h || {
        echo "ITEM-508 inc22: TLS QEMU fixture не снимает" \
            "«$item508_inc22_fixture_undef»" >&2
        exit 1
    }
done

# 7. The documents name the increment and its boundary.
grep -Fq 'ITEM-508 increment 22' docs/ROADMAP.md || {
    echo "ITEM-508 inc22: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 22' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc22: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
