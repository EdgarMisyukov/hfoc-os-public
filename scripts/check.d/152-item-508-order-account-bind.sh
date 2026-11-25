#!/usr/bin/env bash
# ITEM-508 increment 25: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The order driver binds its account key through the production binding point.
#
# Increment 24 built `appliance_acme_account_bind()` but nothing called it: the
# fixture driver still imported the fixture key directly.  This increment makes
# the QEMU proof walk the production path - the driver fills the identity view
# the certificate owner publishes (with the same P-256 key the stub's canned
# chain is for), binds the account key through the module and publishes
# `[ACME-ORDER] account thumbprint=<RFC 7638>` before the account request.
# This slice pins the binding, the record, the runner requirement and the
# documents; the CSR and renewal records are unchanged.
for item508_inc25_file in \
    kernel/src/appliance_acme_order_qemu.c \
    scripts/test-qemu-acme-order.sh \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc25_file ]] || {
        echo "ITEM-508 inc25: отсутствует $item508_inc25_file" >&2
        exit 1
    }
done

# 1. The driver goes through the module; the direct import is gone.
for item508_inc25_driver_pin in \
    '#include "appliance_acme_account.h"' \
    'struct appliance_production_certificate acme_order_identity' \
    'appliance_acme_account_bind(&acme_order_identity, &acme_order_key)' \
    'acme_order_publish_account();' \
    'appliance_acme_account_record()' \
    'the fixture identity account key was refused'; do
    grep -Fq -- "$item508_inc25_driver_pin" \
        kernel/src/appliance_acme_order_qemu.c || {
        echo "ITEM-508 inc25: драйвер не содержит «$item508_inc25_driver_pin»" \
            >&2
        exit 1
    }
done
if grep -Fq 'appliance_acme_key_import(' \
        kernel/src/appliance_acme_order_qemu.c; then
    echo "ITEM-508 inc25: драйвер всё ещё импортирует ключ в обход модуля" >&2
    exit 1
fi
# The account record is published before the account request is built.
item508_inc25_account_line=$(
    grep -n 'acme_order_publish_account();' \
        kernel/src/appliance_acme_order_qemu.c | cut -d: -f1
)
item508_inc25_request_line=$(
    grep -n 'appliance_acme_flow_begin_account(' \
        kernel/src/appliance_acme_order_qemu.c | cut -d: -f1
)
[[ -n $item508_inc25_account_line && -n $item508_inc25_request_line &&
   $item508_inc25_account_line -lt $item508_inc25_request_line ]] || {
    echo "ITEM-508 inc25: запись аккаунта не предшествует запросу" >&2
    exit 1
}

# 2. The serial pair count follows the live driver: seven closed pairs (the
#    seventh is increment 31's listener arming edge).
item508_inc25_begin=$(
    grep -c 'serial_sequence_begin()' kernel/src/appliance_acme_order_qemu.c
)
item508_inc25_end=$(
    grep -c 'serial_sequence_end(' kernel/src/appliance_acme_order_qemu.c
)
[[ $item508_inc25_begin -eq 7 && $item508_inc25_end -eq 7 ]] || {
    echo "ITEM-508 inc25: записи драйвера потеряли пару последовательности" >&2
    exit 1
}

# 3. The Makefile knows the driver's new headers.
grep -Fq 'kernel/include/appliance_acme_account.h' Makefile || {
    echo "ITEM-508 inc25: объект драйвера не зависит от модуля аккаунта" >&2
    exit 1
}
grep -Fq 'kernel/include/appliance_production_certificate.h' Makefile || {
    echo "ITEM-508 inc25: объект драйвера не зависит от вида личности" >&2
    exit 1
}

# 4. The runner requires the account record, not just the success marker.
grep -Fq 'account thumbprint=[A-Za-z0-9_-]{43}' \
    scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc25: прогон не требует записи аккаунта" >&2
    exit 1
}
grep -Fq '[ACME-ORDER] order ok state=CERTIFICATE_VERIFIED' \
    scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc25: прогон не требует проверенной цепочки" >&2
    exit 1
}

# 5. The documents name the increment.
grep -Fq 'ITEM-508 increment 25' docs/ROADMAP.md || {
    echo "ITEM-508 inc25: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 25' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc25: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
