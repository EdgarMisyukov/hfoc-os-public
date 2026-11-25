#!/usr/bin/env bash
# ITEM-508 increment 26: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The ACME service ordinary boot owns.
#
# Increment 24 built the account binding and increment 9 the renewal policy,
# but nothing on the ordinary boot path called either: the service that owns
# the account key and the renewal decision for a real appliance did not exist.
# This increment adds `appliance_production_acme_service`, wires it into
# `kernel.c` (init at boot, poll in the event loop next to serving, one record
# per state) and proves it in the existing certificate host proof.  This slice
# pins the module, the wiring, the proof and the documents; no QEMU proof is
# touched because the service opens no socket and speaks to no CA.
for item508_inc26_file in \
    kernel/include/appliance_production_acme_service.h \
    kernel/src/appliance_production_acme_service.c \
    tests/appliance-certificate-upload-host.c \
    kernel/src/kernel.c \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc26_file ]] || {
        echo "ITEM-508 inc26: отсутствует $item508_inc26_file" >&2
        exit 1
    }
done

# 1. The module is the one owner: bind, policy, bounded record, no key bytes.
for item508_inc26_module_pin in \
    'appliance_production_acme_service_init(' \
    'appliance_production_acme_service_poll(' \
    'appliance_production_acme_service_certificate_obtained(' \
    'appliance_production_acme_service_renewal(' \
    'appliance_acme_account_bind(identity, &service->key)' \
    'appliance_acme_renewal_evaluate(' \
    'appliance_acme_renewal_certificate_obtained(' \
    'appliance_acme_key_thumbprint(&service->key)' \
    '[ACME-SERVICE] v=1 state=' \
    'state=RECORD_TOO_LARGE' \
    'reason=identity_unavailable'; do
    grep -Fq -- "$item508_inc26_module_pin" \
        kernel/src/appliance_production_acme_service.c || {
        echo "ITEM-508 inc26: модуль не содержит «$item508_inc26_module_pin»" \
            >&2
        exit 1
    }
done
# The service must not read or publish key material at all: the thumbprint is
# public, the DER is the account module's business.
if grep -Fq 'key_der' kernel/src/appliance_production_acme_service.c ||
    grep -Fq 'jwk' kernel/src/appliance_production_acme_service.c; then
    echo "ITEM-508 inc26: служба читает байты ключа или JWK" >&2
    exit 1
fi
# The header owns the version, the policy and the three terminal states.
for item508_inc26_header_pin in \
    '#define APPLIANCE_PRODUCTION_ACME_SERVICE_VERSION 1U' \
    'struct appliance_acme_renewal renewal;' \
    'APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_AWAITING_IDENTITY = 2' \
    'APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_BOUND = 3' \
    'APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_REFUSED = 4' \
    'APPLIANCE_PRODUCTION_ACME_SERVICE_RECORD_MAX 224U'; do
    grep -Fq -- "$item508_inc26_header_pin" \
        kernel/include/appliance_production_acme_service.h || {
        echo "ITEM-508 inc26: заголовок не содержит «$item508_inc26_header_pin»" \
            >&2
        exit 1
    }
done

# 2. Ordinary boot owns exactly one service, polls it after serving and
#    publishes its record once per state.
for item508_inc26_kernel_pin in \
    '#include "appliance_production_acme_service.h"' \
    'static struct appliance_production_acme_service production_acme_service;' \
    'appliance_production_acme_service_init(' \
    '&production_acme_service, &production_serving_certificate' \
    'appliance_production_acme_service_poll(' \
    'static void log_production_acme_service(void)' \
    'last_production_acme_service_logged' \
    'last_production_acme_service_record' \
    'console_publish_journal(record, record_length);'; do
    grep -Fq -- "$item508_inc26_kernel_pin" kernel/src/kernel.c || {
        echo "ITEM-508 inc26: kernel.c не содержит «$item508_inc26_kernel_pin»" \
            >&2
        exit 1
    }
done
item508_inc26_init_calls=$(
    grep -cF 'appliance_production_acme_service_init(' kernel/src/kernel.c
)
item508_inc26_poll_calls=$(
    grep -cF 'appliance_production_acme_service_poll(' kernel/src/kernel.c
)
item508_inc26_log_calls=$(
    grep -cF 'log_production_acme_service();' kernel/src/kernel.c
)
[[ $item508_inc26_init_calls -eq 1 &&
   $item508_inc26_poll_calls -eq 1 &&
   $item508_inc26_log_calls -eq 2 ]] || {
    echo "ITEM-508 inc26: служба вызывается не ровно один раз на boot и цикл" >&2
    exit 1
}
# The ACME poll runs after the serving poll, which is what prepares the
# identity the service binds, and the record is logged after that.
item508_inc26_serving_line=$(
    grep -nF 'appliance_production_serving_poll(' kernel/src/kernel.c |
        cut -d: -f1
)
item508_inc26_acme_line=$(
    grep -nF 'appliance_production_acme_service_poll(' kernel/src/kernel.c |
        cut -d: -f1
)
[[ -n $item508_inc26_serving_line && -n $item508_inc26_acme_line &&
   $item508_inc26_serving_line -lt $item508_inc26_acme_line ]] || {
    echo "ITEM-508 inc26: опрос службы не следует за подготовкой личности" >&2
    exit 1
}
# The new record keeps the serial-sequence pair discipline.
item508_inc26_begin=$(grep -cF 'serial_sequence_begin()' kernel/src/kernel.c)
item508_inc26_end=$(grep -cF 'serial_sequence_end(' kernel/src/kernel.c)
[[ $item508_inc26_begin -eq $item508_inc26_end ]] || {
    echo "ITEM-508 inc26: запись службы потеряла пару последовательности" >&2
    exit 1
}

# 3. The host proof drives the service, not just the binding module.
for item508_inc26_test_pin in \
    '#include "appliance_production_acme_service.h"' \
    'static void test_acme_service(void)' \
    'test_acme_service();' \
    'an unprepared identity leaves the ACME service waiting' \
    'the bound service record names a bounded thumbprint' \
    'the account thumbprint survives the reboot byte for byte' \
    'the verified expiry enters the service'"'"'s renewal policy' \
    'a certificate inside the thirty-day window is due' \
    'an unvalidated clock refuses the renewal decision' \
    'a key the JWS module rejects is a named refusal' \
    'the refused service publishes the account refusal' \
    'an uninitialized service refuses to poll and publishes nothing'; do
    grep -Fq -- "$item508_inc26_test_pin" \
        tests/appliance-certificate-upload-host.c || {
        echo "ITEM-508 inc26: проба не содержит «$item508_inc26_test_pin»" >&2
        exit 1
    }
done

# 4. The existing Makefile target links the module and the policy it owns.
for item508_inc26_make_pin in \
    'kernel/include/appliance_acme_renewal.h' \
    'kernel/include/appliance_production_acme_service.h' \
    'kernel/src/appliance_acme_renewal.c' \
    'kernel/src/appliance_production_acme_service.c' \
    'HFOC_APPLIANCE_ACME_SERVICE_HOST_TEST'; do
    grep -Fq -- "$item508_inc26_make_pin" Makefile || {
        echo "ITEM-508 inc26: Makefile не содержит «$item508_inc26_make_pin»" \
            >&2
        exit 1
    }
done

# 5. The documents and the queue name the increment.
grep -Fq 'ITEM-508 increment 26' docs/ROADMAP.md || {
    echo "ITEM-508 inc26: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 26' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc26: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 26' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-508 inc26: очередь не называет инкремент" >&2
    exit 1
}
