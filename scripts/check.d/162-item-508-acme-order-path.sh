#!/usr/bin/env bash
# ITEM-508 increment 35: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal Makefile and shell text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The order path ordinary boot runs: the driver, the boot composition and the
# two consequences the item requires of a verified chain.
#
# Increment 34 gave the operator a durable request; nothing consumed it.  This
# increment adds the driver that drives one complete order over the proven
# transport link and arms the HTTP-01 listener from the parsed challenge, the
# composition that starts it on the operator's request or the renewal policy's
# DUE decision, and the two ends of the success path: the production
# certificate owner adopts the verified chain and the renewal policy moves to
# the leaf's own `notAfter`.  The wire proof of the production driver (a QEMU
# family dialling the stub) is the next increment; this slice pins the host
# proof, the refusals and the boundaries.
for item508_inc35_file in \
    kernel/include/appliance_production_acme_order.h \
    kernel/src/appliance_production_acme_order.c \
    kernel/include/appliance_production_acme_order_boot.h \
    kernel/src/appliance_production_acme_order_boot.c \
    kernel/include/appliance_acme_trust.h \
    kernel/src/appliance_acme_trust.c \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/src/kernel.c \
    tests/appliance-production-acme-order-host.c \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc35_file ]] || {
        echo "ITEM-508 inc35: отсутствует $item508_inc35_file" >&2
        exit 1
    }
done

# 1. The driver: named states, named refusals, one bounded record, the DER
#    split and the two success consequences.  It owns no socket.
for item508_inc35_driver_pin in \
    'APPLIANCE_PRODUCTION_ACME_ORDER_STATE_DONE' \
    'APPLIANCE_PRODUCTION_ACME_ORDER_BAD_CLOCK' \
    'APPLIANCE_PRODUCTION_ACME_ORDER_NO_IDENTIFIERS' \
    'APPLIANCE_PRODUCTION_ACME_ORDER_NO_LISTENER' \
    'APPLIANCE_PRODUCTION_ACME_ORDER_CHAIN_REFUSED' \
    'APPLIANCE_PRODUCTION_ACME_ORDER_TIMEOUT' \
    'appliance_production_acme_order_der_length' \
    'appliance_production_certificate_adopt_acme' \
    'appliance_acme_renewal_certificate_obtained' \
    'reason=NO_IDENTIFIERS'; do
    grep -Fq -- "$item508_inc35_driver_pin" \
        kernel/include/appliance_production_acme_order.h \
        kernel/src/appliance_production_acme_order.c || {
        echo "ITEM-508 inc35: драйвер не содержит «$item508_inc35_driver_pin»" \
            >&2
        exit 1
    }
done
grep -Fq '[PRODUCTION-ACME-ORDER] state=' \
    kernel/src/appliance_production_acme_order.c || {
    echo "ITEM-508 inc35: драйвер не публикует запись" >&2
    exit 1
}
if grep -Eq '#include "(lwip|mbedtls)/' \
    kernel/src/appliance_production_acme_order.c; then
    echo "ITEM-508 inc35: драйвер тянет сокет или TLS вместо инъекции" >&2
    exit 1
fi

# 2. The boot composition: one binding, the production endpoint, the request
#    spent durably, the renewal accounting, and the TLS-profile membership.
for item508_inc35_boot_pin in \
    'appliance_production_acme_order_boot_configure' \
    'appliance_production_acme_order_boot_tick' \
    'appliance_acme_transport_lwip_init' \
    'appliance_production_serving_acme_control' \
    'appliance_production_serving_acme_take_request' \
    'appliance_production_serving_acme_identifier' \
    'appliance_acme_renewal_attempt_started' \
    'appliance_acme_renewal_attempt_failed' \
    'appliance_production_acme_http01_listening' \
    'https://acme-v02.api.letsencrypt.org/directory'; do
    grep -Fq -- "$item508_inc35_boot_pin" \
        kernel/src/appliance_production_acme_order_boot.c || {
        echo "ITEM-508 inc35: композиция не содержит «$item508_inc35_boot_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'kernel/src/appliance_production_acme_order_boot.c' Makefile || {
    echo "ITEM-508 inc35: композиция не входит в сборку ядра" >&2
    exit 1
}
grep -Fq '	$(MAKE) test-appliance-production-acme-order-host' Makefile || {
    echo "ITEM-508 inc35: хост-набор не исполняет пробу драйвера" >&2
    exit 1
}

# 3. The ordinary loop only publishes, ticks and mirrors the record; the
#    serving side hands out the identifiers and spends the request.
for item508_inc35_loop_pin in \
    'appliance_production_acme_order_boot_publish' \
    'appliance_production_acme_order_boot_tick' \
    'appliance_production_acme_order_boot_record' \
    'log_production_acme_order();'; do
    grep -Fq -- "$item508_inc35_loop_pin" kernel/src/kernel.c || {
        echo "ITEM-508 inc35: обычная загрузка не содержит «$item508_inc35_loop_pin»" \
            >&2
        exit 1
    }
done
for item508_inc35_serving_pin in \
    'appliance_production_serving_acme_take_request' \
    'appliance_production_serving_acme_identifier_count' \
    'appliance_production_serving_acme_identifier('; do
    grep -Fq -- "$item508_inc35_serving_pin" \
        kernel/include/appliance_production_serving.h \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-508 inc35: serving не содержит «$item508_inc35_serving_pin»" \
            >&2
        exit 1
    }
done

# 4. The trust module publishes the verified leaf's own notAfter and never
#    leaves one behind after a refusal; the host proof pins R10's expiry.
for item508_inc35_trust_pin in \
    'leaf_not_after_epoch_seconds' \
    'appliance_acme_trust_leaf_not_after'; do
    grep -Fq -- "$item508_inc35_trust_pin" \
        kernel/include/appliance_acme_trust.h \
        kernel/src/appliance_acme_trust.c || {
        echo "ITEM-508 inc35: доверие не содержит «$item508_inc35_trust_pin»" \
            >&2
        exit 1
    }
done
for item508_inc35_trust_proof in \
    'the verified leaf publishes its own notAfter' \
    'UINT32_C(1804895999)' \
    'a refused chain publishes no leaf expiry' \
    'a foreign anchor publishes no leaf expiry'; do
    grep -Fq -- "$item508_inc35_trust_proof" \
        tests/appliance-acme-trust-host.c || {
        echo "ITEM-508 inc35: проба доверия не утверждает «$item508_inc35_trust_proof»" \
            >&2
        exit 1
    }
done

# 5. The host proof drives the whole order over a scripted link and checks the
#    success consequences and the refusals.
for item508_inc35_proof_pin in \
    'struct scripted_link' \
    'the scripted order reaches the terminal DONE state' \
    'the listener was armed once from the parsed challenge token' \
    'the renewal policy moved to the verified leaf expiry' \
    'the adopted leaf is the certificate the order downloaded' \
    'the loaded record keeps the ACME provenance' \
    'a chain under a foreign anchor fails the order by name' \
    'a challenge no listener can answer fails the order by name' \
    'an order that outlives its backstop fails instead of waiting' \
    'the order record carries the expiry and no token or nonce'; do
    grep -Fq -- "$item508_inc35_proof_pin" \
        tests/appliance-production-acme-order-host.c || {
        echo "ITEM-508 inc35: проба драйвера не содержит «$item508_inc35_proof_pin»" \
            >&2
        exit 1
    }
done

# 6. The documents name the increment.
for item508_inc35_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-508 increment 35' "$item508_inc35_doc" || {
        echo "ITEM-508 inc35: $item508_inc35_doc не называет инкремент" >&2
        exit 1
    }
done
