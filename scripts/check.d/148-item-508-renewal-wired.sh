#!/usr/bin/env bash
# ITEM-508 increment 20: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The verified certificate feeds the renewal policy.
#
# `appliance_acme_renewal` (increment 9) was host-proven but no order ever
# reached it: the fixture proof stopped at `CERTIFICATE_VERIFIED`.  This slice
# pins that the one call which moves the expiry is the verified chain, that the
# policy then says `NOT_NEEDED` at the order's instant, `DUE` once its 30-day
# window has opened, and `RETRY_AFTER` after one failed attempt (increment 21),
# and that every decision is published in the driver's records - without a
# second serial pair and without touching the flow, the stub or the harness.
for item508_inc20_file in \
    kernel/src/appliance_acme_order_qemu.c \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc20_file ]] || {
        echo "ITEM-508 inc20: отсутствует $item508_inc20_file" >&2
        exit 1
    }
done

# 1. The driver drives the renewal policy: the named fixture lifetime, the one
#    expiry-moving call, both evaluations, the record and the decisions.
for item508_inc20_pin in \
    '#include "appliance_acme_renewal.h"' \
    'APPLIANCE_ACME_ORDER_QEMU_RENEWAL_SECONDS' \
    'appliance_acme_renewal_init(&acme_order_renewal)' \
    'appliance_acme_renewal_certificate_obtained(' \
    'appliance_acme_renewal_evaluate(' \
    'APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED' \
    'APPLIANCE_ACME_RENEWAL_DECISION_DUE' \
    'APPLIANCE_ACME_RENEWAL_WINDOW_SECONDS' \
    'appliance_acme_renewal_record(&acme_order_renewal)' \
    'appliance_acme_renewal_decision_string(' \
    'the renewal policy refused the certificate' \
    'the fresh certificate is not fresh' \
    'the renewal window did not open' \
    'appliance_acme_renewal_attempt_started(' \
    'appliance_acme_renewal_attempt_failed(' \
    'APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER' \
    'appliance_acme_renewal_attempts(' \
    'the renewal attempt did not start' \
    'the renewal attempt did not finish' \
    'the failed attempt is not in its retry window' \
    'char due_record[APPLIANCE_ACME_RENEWAL_RECORD_MAX]' \
    'serial_write(due_record)'; do
    grep -Fq -- "$item508_inc20_pin" \
        kernel/src/appliance_acme_order_qemu.c || {
        echo "ITEM-508 inc20: драйвер не содержит «$item508_inc20_pin»" >&2
        exit 1
    }
done
item508_inc20_evaluations=$(
    grep -c 'appliance_acme_renewal_evaluate(' \
        kernel/src/appliance_acme_order_qemu.c
)
[[ $item508_inc20_evaluations -eq 3 ]] || {
    echo "ITEM-508 inc20: политика оценивается не трижды (свежая, окно, повтор)" >&2
    exit 1
}
# The renewal lines stay inside the existing record pair: the serial pair count
# of slices 145/147 must not move for a reporting change.  ITEM-508 increment
# 22 added the fifth pair - the CSR record written before the finalize request
# - increment 25 the sixth (the account record written before the account
# request) and increment 31 the seventh (the listener's arming edge) - so the
# count is seven, and every pair is still closed.
item508_inc20_begin=$(
    grep -c 'serial_sequence_begin()' kernel/src/appliance_acme_order_qemu.c
)
item508_inc20_end=$(
    grep -c 'serial_sequence_end(' kernel/src/appliance_acme_order_qemu.c
)
[[ $item508_inc20_begin -eq 7 && $item508_inc20_end -eq 7 ]] || {
    echo "ITEM-508 inc20: записи драйвера потеряли пару последовательности" >&2
    exit 1
}
grep -Fq '"[ACME-ORDER] renewal decision="' \
    kernel/src/appliance_acme_order_qemu.c || {
    echo "ITEM-508 inc20: решение продления не публикуется" >&2
    exit 1
}
grep -Fq '" expires="' kernel/src/appliance_acme_order_qemu.c || {
    echo "ITEM-508 inc20: свежее решение не называет срок" >&2
    exit 1
}
grep -Fq 'appliance_acme_renewal_expires_at(' \
    kernel/src/appliance_acme_order_qemu.c || {
    echo "ITEM-508 inc20: срок берётся не у политики" >&2
    exit 1
}
grep -Fq '\n[ACME-ORDER] order ok state="' \
    kernel/src/appliance_acme_order_qemu.c || {
    echo "ITEM-508 inc20: запись об успехе заказа исчезла" >&2
    exit 1
}

# 2. The Makefile knows the new dependency of the driver object.
grep -Fq 'kernel/include/appliance_acme_renewal.h' Makefile || {
    echo "ITEM-508 inc20: объект драйвера не зависит от заголовка продления" >&2
    exit 1
}

# 3. The harness marker is unchanged: the proof still ends with the verified
#    chain, the renewal records only precede it.
grep -Fq '[ACME-ORDER] order ok state=CERTIFICATE_VERIFIED' \
    scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc20: прогон не требует проверенной цепочки" >&2
    exit 1
}

# 4. The documents name the increment and the boundary it leaves.
grep -Fq 'ITEM-508 increment 20' docs/ROADMAP.md || {
    echo "ITEM-508 inc20: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 20' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc20: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
