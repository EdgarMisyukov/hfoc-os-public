#!/usr/bin/env bash
# ITEM-508 increment 17: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The offline ACME order: one complete order through the real binding.
#
# Increment 17 drives the increment-5 flow over the increment-11 transport and
# the increment-13 binding, against the grown offline stub.  This slice keeps
# the proof honest in the places a later edit could quietly hollow out:
#
#   * the order family is a family of the production kernel: production
#     objects, the production anchor filtered out of the link, its own fixed
#     port (both proofs can run concurrently), and the production kernel/ISO
#     must not carry `[ACME-ORDER]` before or after the harness;
#   * the certificate download is a real POST-as-GET built by the flow
#     (`appliance_acme_flow_begin_certificate`), not a plain GET;
#   * every document the stub serves has one source of truth (the fixture
#     header) and every reply carries a `Replay-Nonce`, which is what the
#     flow's next request needs;
#   * the proof epoch is a named fixture constant, and the flow still refuses
#     an unvalidated clock;
#   * the driver's records are single lines and name the stage, the flow
#     state and the transport status on a refusal.
for item508_inc17_file in \
    Makefile \
    kernel/include/appliance_acme_flow.h \
    kernel/src/appliance_acme_flow.c \
    kernel/src/appliance_acme_order_qemu.c \
    kernel/src/kernel.c \
    kernel/src/appliance_acme_directory_anchor_fixture.c \
    scripts/hfoc-acme-directory-stub.py \
    scripts/test-acme-directory-stub-host.sh \
    scripts/test-qemu-acme-order.sh \
    tests/appliance-acme-flow-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc17_file ]] || {
        echo "ITEM-508 inc17: отсутствует $item508_inc17_file" >&2
        exit 1
    }
done

# 1. The order family is a family of the production kernel, and only of it.
for item508_inc17_make_name in \
    'ACME_ORDER_OBJ_DIR' \
    'ACME_ORDER_CFLAGS' \
    'ACME_ORDER_OBJECTS' \
    'ACME_ORDER_KERNEL' \
    'ACME_ORDER_ISO' \
    'ACME_ORDER_DEPS'; do
    grep -Fq -- "$item508_inc17_make_name" Makefile || {
        echo "ITEM-508 inc17: Makefile не объявляет $item508_inc17_make_name" >&2
        exit 1
    }
done
grep -Fq -- '-DHFOC_APPLIANCE_ACME_ORDER_QEMU_TEST' Makefile || {
    echo "ITEM-508 inc17: семейство не определяет макрос пробы" >&2
    exit 1
}
item508_inc17_objects=$(
    awk '/^ACME_ORDER_OBJECTS[[:space:]]*:=/ { inside = 1 }
         inside { print }
         inside && $0 !~ /\\$/ { exit }' Makefile
)
# shellcheck disable=SC2016  # the compared text is literal, not expanded
[[ $item508_inc17_objects == *'$(filter-out'* ]] || {
    echo "ITEM-508 inc17: объекты семейства не исключают production-якорь" >&2
    exit 1
}
for item508_inc17_excluded in \
    'build/obj/appliance_acme_trust_anchor.o' \
    'build/obj/kernel.o'; do
    [[ $item508_inc17_objects == *"$item508_inc17_excluded"* ]] || {
        echo "ITEM-508 inc17: из компоновки не исключён $item508_inc17_excluded" >&2
        exit 1
    }
done

# 2. The production marker checks stand on both sides of the harness, and the
#    proof runs in the acceptance suite.
item508_inc17_target=$(
    awk '/^test-acme-order-qemu:/ { inside = 1 }
         inside { print }
         inside && /^$/ { exit }' Makefile
)
[[ -n $item508_inc17_target ]] || {
    echo "ITEM-508 inc17: нет цели test-acme-order-qemu" >&2
    exit 1
}
for item508_inc17_assert in \
    "strings \$(ACME_ORDER_KERNEL) | grep -Fq '[ACME-ORDER] proof init'" \
    "strings \$(ACME_ORDER_ISO) | grep -Fq '[ACME-ORDER] proof init'" \
    "if strings \$(KERNEL) | grep -Fq '[ACME-ORDER]'" \
    "if strings \$(IMAGE) | grep -Fq '[ACME-ORDER]'" \
    "HFOC_ACME_ORDER_QEMU_TEST_PORT='18446'" \
    './scripts/test-qemu-acme-order.sh'; do
    [[ $item508_inc17_target == *"$item508_inc17_assert"* ]] || {
        echo "ITEM-508 inc17: цель не содержит «$item508_inc17_assert»" >&2
        exit 1
    }
done
item508_inc17_kernel_asserts=$(
    grep -c 'FAIL: production kernel contains the order proof marker' \
        <<<"$item508_inc17_target"
)
item508_inc17_iso_asserts=$(
    grep -c 'FAIL: production ISO contains the order proof marker' \
        <<<"$item508_inc17_target"
)
[[ $item508_inc17_kernel_asserts -eq 2 && $item508_inc17_iso_asserts -eq 2 ]] || {
    echo "ITEM-508 inc17: проверка production-маркера стоит не с обеих сторон прогона" >&2
    exit 1
}
item508_inc17_phase=$(
    awk '/^QEMU_PARALLEL_PHASE_1[[:space:]]*:=/ { inside = 1 }
         inside { print }
         inside && $0 !~ /\\$/ { exit }' Makefile
)
[[ $item508_inc17_phase == *test-acme-order-qemu* ]] || {
    echo "ITEM-508 inc17: проба не входит в QEMU_PARALLEL_PHASE_1" >&2
    exit 1
}

# 3. One fixed port (its own: the directory proof keeps 18445) on both sides.
grep -Fq '#define APPLIANCE_ACME_ORDER_QEMU_PORT 18446U' \
    kernel/src/appliance_acme_order_qemu.c || {
    echo "ITEM-508 inc17: драйвер не несёт свой фиксированный порт" >&2
    exit 1
}
# shellcheck disable=SC2016  # the compared text is literal, not expanded
grep -Fq 'PORT=${HFOC_ACME_ORDER_QEMU_TEST_PORT:-18446}' \
    scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc17: прогон не несёт общий порт" >&2
    exit 1
}

# 4. The driver is a fixture driver of the flow, not a second implementation:
#    every step goes through the flow, the key and the anchor are the fixture
#    ones, the RNG is the production entropy source, and the epoch is a named
#    fixture constant.
grep -Fq '#ifdef HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST' \
    kernel/src/appliance_acme_order_qemu.c || {
    echo "ITEM-508 inc17: драйвер не ограждён макросом пробы" >&2
    exit 1
}
grep -Fq 'appliance_acme_order_qemu_translation_unit_guard' \
    kernel/src/appliance_acme_order_qemu.c || {
    echo "ITEM-508 inc17: пустой перевод драйвера не защищён от предупреждения" >&2
    exit 1
}
for item508_inc17_call in \
    'appliance_acme_flow_begin(' \
    'appliance_acme_flow_declare_identifiers(' \
    'appliance_acme_flow_begin_account(' \
    'appliance_acme_flow_account_result(' \
    'appliance_acme_flow_begin_order(' \
    'appliance_acme_flow_order_result(' \
    'appliance_acme_flow_begin_authorization(' \
    'appliance_acme_flow_authorization_result(' \
    'appliance_acme_flow_begin_challenge(' \
    'appliance_acme_flow_challenge_result(' \
    'appliance_acme_flow_begin_finalize(' \
    'appliance_acme_flow_finalize_result(' \
    'appliance_acme_flow_begin_certificate(' \
    'appliance_acme_flow_certificate_result(' \
    'appliance_production_entropy_random' \
    'directory_fixture_leaf_key' \
    'appliance_acme_trust_anchor_der_data(' \
    'APPLIANCE_ACME_ORDER_QEMU_EPOCH' \
    '[ACME-ORDER] order ok state='; do
    grep -Fq -- "$item508_inc17_call" \
        kernel/src/appliance_acme_order_qemu.c || {
        echo "ITEM-508 inc17: драйвер не содержит «$item508_inc17_call»" >&2
        exit 1
    }
done
# ITEM-508 increment 31: the fixture epoch itself moved to the shared fixture
# clock unit, so this slice pins the driver's spelling of it (the macro above)
# and that the value comes from that one accessor; slice 158 pins that the
# literal exists exactly once in the kernel tree.
grep -Fq 'appliance_acme_fixture_epoch_seconds()' \
    kernel/src/appliance_acme_order_qemu.c || {
    echo "ITEM-508 inc17: эпоха пробы не названа константой" >&2
    exit 1
}
item508_inc17_begin=$(
    grep -c 'serial_sequence_begin()' kernel/src/appliance_acme_order_qemu.c
)
item508_inc17_end=$(
    grep -c 'serial_sequence_end(' kernel/src/appliance_acme_order_qemu.c
)
# The count moves with the driver's records; ITEM-508 increment 22 added the
# fifth pair (the CSR record written before the finalize request), increment 25
# the sixth (the account record written before the account request) and
# increment 31 the seventh (the listener's arming edge, armed / armed=0 served).
[[ $item508_inc17_begin -eq 7 && $item508_inc17_end -eq 7 ]] || {
    echo "ITEM-508 inc17: записи драйвера не обрамлены парами последовательности" >&2
    exit 1
}
grep -Fq ' tls=' kernel/src/appliance_acme_order_qemu.c || {
    echo "ITEM-508 inc17: запись отказа не называет ошибку TLS" >&2
    exit 1
}
grep -Fq 'appliance_acme_order_qemu_test_tick' kernel/src/kernel.c || {
    echo "ITEM-508 inc17: главный цикл не двигает драйвер" >&2
    exit 1
}

# 5. The certificate step is a real POST-as-GET built by the flow, with the
#    empty payload and the account's `kid`; the host proof drives it.
grep -Fq 'appliance_acme_flow_begin_certificate' \
    kernel/include/appliance_acme_flow.h || {
    echo "ITEM-508 inc17: заголовок потока не публикует шаг сертификата" >&2
    exit 1
}
grep -Fq 'appliance_acme_flow_begin_certificate' \
    kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc17: поток не реализует шаг сертификата" >&2
    exit 1
}
for item508_inc17_case in \
    'the certificate POST-as-GET request is built' \
    'the certificate POST-as-GET carries the empty payload' \
    'the certificate request is out of order after it verified'; do
    grep -Fq -- "$item508_inc17_case" tests/appliance-acme-flow-host.c || {
        echo "ITEM-508 inc17: проба потока не проверяет «$item508_inc17_case»" >&2
        exit 1
    }
done

# 6. The stub serves the whole order from the one fixture chain, and every
#    reply carries the nonce the next request needs.
for item508_inc17_endpoint in \
    '"/acme/new-nonce"' \
    '"/acme/new-account"' \
    '"/acme/new-order"' \
    '"/acme/authz/1"' \
    '"/acme/chall/1"' \
    '"/acme/finalize/1"' \
    '"/acme/cert/1"' \
    '"Replay-Nonce: %s' \
    '"application/pem-certificate-chain"' \
    'self.chain_pem'; do
    grep -Fq -- "$item508_inc17_endpoint" \
        scripts/hfoc-acme-directory-stub.py || {
        echo "ITEM-508 inc17: заглушка не содержит «$item508_inc17_endpoint»" >&2
        exit 1
    }
done
# The directory document the increment-15 driver compares byte for byte must
# stay the one the stub serves: both sides name the same three endpoints.
for item508_inc17_directory in \
    'acme/new-nonce' \
    'acme/new-account' \
    'acme/new-order'; do
    grep -Fq -- "$item508_inc17_directory" \
        kernel/src/appliance_acme_directory_qemu.c || {
        echo "ITEM-508 inc17: драйвер инкремента 15 не знает «$item508_inc17_directory»" >&2
        exit 1
    }
done

# 7. The host proof exercises every endpoint before any guest boots.
for item508_inc17_request in \
    'stub_request GET /directory' \
    'stub_request POST /acme/new-account' \
    'stub_request POST /acme/new-order' \
    'stub_request POST /acme/authz/1' \
    'stub_request POST /acme/chall/1' \
    'stub_request POST /acme/finalize/1' \
    'stub_request POST /acme/cert/1'; do
    grep -Fq -- "$item508_inc17_request" \
        scripts/test-acme-directory-stub-host.sh || {
        echo "ITEM-508 inc17: хост-проба не делает «$item508_inc17_request»" >&2
        exit 1
    }
done

# 8. The harness waits for the verified chain and fails on a named refusal.
grep -Fq '[ACME-ORDER] order ok state=CERTIFICATE_VERIFIED' \
    scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc17: прогон не требует проверенной цепочки" >&2
    exit 1
}
grep -Fq '[ACME-ORDER] failed:' scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc17: прогон не падает на записи отказа" >&2
    exit 1
}
# shellcheck disable=SC2016  # the compared text is literal, not expanded
grep -Fq 'scripts/hfoc-acme-directory-stub.py --port "$PORT"' \
    scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc17: прогон не поднимает заглушку" >&2
    exit 1
}

# 9. The documents name the increment and its honest bound.
grep -Fq 'ITEM-508 increment 17' docs/ROADMAP.md || {
    echo "ITEM-508 inc17: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 17' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc17: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
