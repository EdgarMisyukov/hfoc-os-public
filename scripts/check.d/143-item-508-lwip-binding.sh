#!/usr/bin/env bash
# ITEM-508 increment 13: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The lwIP and mbedTLS binding of the ACME transport link.
#
# The driver owns every decision; the binding only moves bytes over a real
# connection and a real TLS 1.3 client handshake.  This slice pins the one
# permitted call site of the internal client handshake symbol, the profile
# registration that gives the binding the TLS headers, and the contracts that
# keep the binding from re-deciding what the driver already decided.

for item508_inc13_file in \
    kernel/include/appliance_acme_transport_lwip.h \
    kernel/src/appliance_acme_transport_lwip.c \
    Makefile \
    scripts/check.d/01-lwip-source-license-lock.sh \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc13_file ]] || {
        echo "ITEM-508 inc13: отсутствует $item508_inc13_file" >&2
        exit 1
    }
done

# 1. The one permitted call site.  The verifier is invoked with the exception
#    that names exactly this pair, and with no second exception: the mechanism
#    stays a single checked pair, not a general repeal.
grep -Fq -- '--except=mbedtls_ssl_handshake_step:kernel/src/appliance_acme_transport_lwip.c' \
    scripts/check.d/01-lwip-source-license-lock.sh || {
    echo "ITEM-508 inc13: срез 01 не передаёт проверяемое исключение" >&2
    exit 1
}
item508_inc13_exceptions=$(
    grep -c -- '--except=' scripts/check.d/01-lwip-source-license-lock.sh
)
[[ $item508_inc13_exceptions -eq 1 ]] || {
    echo "ITEM-508 inc13: исключений должно быть ровно одно, их $item508_inc13_exceptions" >&2
    exit 1
}
# The binding is a checked source of that invocation, not a dangling exception.
grep -Fq 'kernel/src/appliance_acme_transport_lwip.c' \
    scripts/check.d/01-lwip-source-license-lock.sh || {
    echo "ITEM-508 inc13: привязка не названа среди проверяемых исходников" >&2
    exit 1
}

# 2. No other kernel source calls the client handshake symbol, and the
#    binding is the only source that even names it outside the verifier.
item508_inc13_callers=$(
    grep -rlnE 'mbedtls_ssl_handshake_(client_)?step' kernel/src kernel/include 2>/dev/null |
        grep -v '^kernel/src/appliance_acme_transport_lwip.c$' || true
)
[[ -z $item508_inc13_callers ]] || {
    echo "ITEM-508 inc13: клиентский символ зовут посторонние исходники:" >&2
    echo "$item508_inc13_callers" >&2
    exit 1
}

# 3. The TLS profile registration: the binding needs the TLS profile header and
#    the mbedTLS include path, and the list that grants them is explicit.
grep -Eq 'HFOC_TLS_PROFILE_KERNEL_SOURCES[[:space:]]*:=' Makefile || {
    echo "ITEM-508 inc13: Makefile не объявляет список TLS-профиля" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_acme_transport_lwip.c' Makefile || {
    echo "ITEM-508 inc13: привязка не зарегистрирована в TLS-профиле" >&2
    exit 1
}

# 4. The five link operations exist and are published, and the binding keeps
#    the decisions the driver owns: it verifies the peer, it speaks exactly
#    TLS 1.3, it parses the image's one anchor, it asks the live lifecycle's
#    resolver for a name, and it maps every terminal result by name.
for item508_inc13_pin in \
    'binding_link_open' \
    'binding_link_advance' \
    'binding_link_send' \
    'binding_link_recv' \
    'binding_link_close' \
    'mbedtls_ssl_conf_authmode(' \
    'MBEDTLS_SSL_VERIFY_REQUIRED' \
    'MBEDTLS_SSL_VERSION_TLS1_3' \
    'mbedtls_x509_crt_parse_der(' \
    'appliance_production_entropy_random' \
    'appliance_network_lifecycle_dns_query(' \
    'appliance_dns_resolver_state(' \
    'hfoc_mbedtls_bio_attach(' \
    'hfoc_mbedtls_alloc_connection_open(' \
    'hfoc_mbedtls_alloc_connection_enter(' \
    'hfoc_mbedtls_alloc_connection_exit()' \
    'tcp_connect(' \
    'tcp_recv(' \
    'tcp_err(' \
    'tcp_sndbuf(' \
    'tcp_recved(' \
    'TCP_WRITE_FLAG_COPY' \
    'APPLIANCE_ACME_TRANSPORT_LWIP_ANCHOR_REFUSED' \
    'mbedtls_ssl_handshake_step('; do
    grep -Fq -- "$item508_inc13_pin" \
        kernel/src/appliance_acme_transport_lwip.c || {
        echo "ITEM-508 inc13: привязка не содержит «$item508_inc13_pin»" >&2
        exit 1
    }
done
# Every other kernel build compiles the wildcard sources without the TLS
# profile; both halves must be empty there instead of leaking the dependency.
grep -Fq '#ifdef HFOC_PRODUCTION_TLS' \
    kernel/include/appliance_acme_transport_lwip.h || {
    echo "ITEM-508 inc13: заголовок привязки не ограждён TLS-профилем" >&2
    exit 1
}
grep -Fq '#ifdef HFOC_PRODUCTION_TLS' \
    kernel/src/appliance_acme_transport_lwip.c || {
    echo "ITEM-508 inc13: исходник привязки не ограждён TLS-профилем" >&2
    exit 1
}
grep -Fq 'appliance_acme_transport_lwip_translation_unit_guard' \
    kernel/src/appliance_acme_transport_lwip.c || {
    echo "ITEM-508 inc13: пустой не-TLS перевод не защищён от предупреждения" >&2
    exit 1
}

# The binding must not log: the record belongs to the driver above it.
if grep -Eq '\b(serial_printf|console_write|serial_write)\b' \
        kernel/src/appliance_acme_transport_lwip.c; then
    echo "ITEM-508 inc13: привязка публикует собственную запись" >&2
    exit 1
fi

# 5. The documents name the increment and its bound.
for item508_inc13_doc in docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Fq 'ITEM-508 increment 13' "$item508_inc13_doc" || {
        echo "ITEM-508 inc13: $item508_inc13_doc не называет инкремент" >&2
        exit 1
    }
done
