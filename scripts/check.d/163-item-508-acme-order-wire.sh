#!/usr/bin/env bash
# ITEM-508 increment 36: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal Makefile and shell text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The production order path over the real binding.
#
# Increment 35 proved the production driver on the host with a scripted link;
# this increment proves it on a wire.  A new QEMU family is the production
# kernel plus the fixture anchor and the shared fixture clock, its own port
# keeps it parallel to the fixture order proof, the boot composition inside it
# dials the offline stub and the fixture entry asks for exactly one order, and
# the harness requires the production records: the driver runs, the listener
# arms from the parsed challenge, the verified chain is adopted and the renewal
# policy moves to the leaf's expiry - with no failure, no stub refusal and no
# private key in either log.
for item508_inc36_file in \
    scripts/test-qemu-acme-boot-order.sh \
    kernel/src/appliance_production_acme_order_boot.c \
    kernel/include/appliance_production_acme_order_boot.h \
    kernel/src/appliance_acme_directory_anchor_fixture.c \
    kernel/src/appliance_acme_fixture_clock_qemu.c \
    kernel/src/kernel.c \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc36_file ]] || {
        echo "ITEM-508 inc36: отсутствует $item508_inc36_file" >&2
        exit 1
    }
done

# 1. The family: production objects plus the fixture anchor and clock, its own
#    port, and the wire-proof target in the parallel QEMU phase.
for item508_inc36_family_pin in \
    'ACME_BOOT_ORDER_OBJ_DIR' \
    'ACME_BOOT_ORDER_CFLAGS' \
    'ACME_BOOT_ORDER_OBJECTS' \
    'ACME_BOOT_ORDER_KERNEL' \
    'ACME_BOOT_ORDER_ISO' \
    'HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST' \
    'HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_URL'; do
    grep -Fq -- "$item508_inc36_family_pin" Makefile || {
        echo "ITEM-508 inc36: Makefile не объявляет «$item508_inc36_family_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'test-acme-boot-order-qemu: $(ACME_BOOT_ORDER_ISO) iso' Makefile || {
    echo "ITEM-508 inc36: нет цели проводной пробы" >&2
    exit 1
}
grep -Fq "HFOC_ACME_BOOT_ORDER_QEMU_TEST_PORT='18449'" Makefile || {
    echo "ITEM-508 inc36: цель не называет свой порт" >&2
    exit 1
}
grep -Fq '	test-acme-boot-order-qemu \' Makefile || {
    echo "ITEM-508 inc36: проба не входит в параллельную QEMU-фазу" >&2
    exit 1
}

# 2. The harness requires the production records and refuses to pass on a
#    failure, a stub refusal or key material.
for item508_inc36_harness_pin in \
    'PRODUCTION-ACME-ORDER' \
    'state=DONE step=CERTIFICATE' \
    'state=FAILED' \
    'armed=1' \
    'ACME-STUB refusal:' \
    'PRIVATE KEY' \
    'QEMU exited before the order exchange'; do
    grep -Fq -- "$item508_inc36_harness_pin" \
        scripts/test-qemu-acme-boot-order.sh || {
        echo "ITEM-508 inc36: проба провода не содержит «$item508_inc36_harness_pin»" \
            >&2
        exit 1
    }
done
[[ -x scripts/test-qemu-acme-boot-order.sh ]] || {
    echo "ITEM-508 inc36: проба провода не исполняема" >&2
    exit 1
}

# 3. The boot composition: the fixture entry, the stub URL branch, the identity
#    gate and the fixture identity the guest has no operator to prepare.
for item508_inc36_boot_pin in \
    'appliance_production_acme_order_boot_fixture_request' \
    'HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_URL' \
    'appliance_production_certificate_available(boot_identity)' \
    'boot_fixture_requested'; do
    grep -Fq -- "$item508_inc36_boot_pin" \
        kernel/src/appliance_production_acme_order_boot.c \
        kernel/include/appliance_production_acme_order_boot.h || {
        echo "ITEM-508 inc36: композиция не содержит «$item508_inc36_boot_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'appliance_production_acme_order_boot_fixture_request();' \
    kernel/src/kernel.c || {
    echo "ITEM-508 inc36: обычная загрузка не просит заказ в семье пробы" >&2
    exit 1
}
grep -Fq 'appliance_production_certificate_prepare(' kernel/src/kernel.c || {
    echo "ITEM-508 inc36: гость пробы не готовит личность учётной записи" >&2
    exit 1
}
grep -Fq 'appliance_acme_fixture_epoch_seconds();' kernel/src/kernel.c || {
    echo "ITEM-508 inc36: заказ не получает общие фикстурные часы" >&2
    exit 1
}

# 4. The shared fixture objects are compiled in the new family too.
grep -Fq 'HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST' \
    kernel/src/appliance_acme_fixture_clock_qemu.c || {
    echo "ITEM-508 inc36: общие часы не знают семью" >&2
    exit 1
}
grep -Fq 'HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST' \
    kernel/src/appliance_acme_directory_anchor_fixture.c || {
    echo "ITEM-508 inc36: фикстурный якорь не знает семью" >&2
    exit 1
}

# 5. The documents name the increment.
for item508_inc36_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-508 increment 36' "$item508_inc36_doc" || {
        echo "ITEM-508 inc36: $item508_inc36_doc не называет инкремент" >&2
        exit 1
    }
done
