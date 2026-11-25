#!/usr/bin/env bash
# ITEM-508 increment 19: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The ACME client polls a pending order too.
#
# After finalize a real CA may answer with the order still pending; RFC 8555
# section 7.4 polls the order URL - the `Location` of the newOrder reply -
# with POST-as-GET until it is valid.  This slice pins that the flow keeps the
# order URL, that the poll is a real POST-as-GET, that the fixture driver takes
# the poll path, and that the offline stub names the order URL and answers the
# poll with the finalized order.
for item508_inc19_file in \
    kernel/include/appliance_acme_flow.h \
    kernel/src/appliance_acme_flow.c \
    kernel/src/appliance_acme_order_qemu.c \
    scripts/hfoc-acme-directory-stub.py \
    scripts/test-acme-directory-stub-host.sh \
    scripts/test-qemu-acme-order.sh \
    tests/appliance-acme-flow-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc19_file ]] || {
        echo "ITEM-508 inc19: отсутствует $item508_inc19_file" >&2
        exit 1
    }
done

# 1. The flow keeps the order URL and publishes the poll; the request is a
#    POST-as-GET (empty payload, the account's `kid`) allowed only while the
#    finalize is pending.
grep -Fq 'struct appliance_acme_url order_url;' \
    kernel/include/appliance_acme_flow.h || {
    echo "ITEM-508 inc19: поток не хранит URL заказа" >&2
    exit 1
}
grep -Fq 'const char *order_url,' kernel/include/appliance_acme_flow.h || {
    echo "ITEM-508 inc19: order_result не принимает URL заказа" >&2
    exit 1
}
grep -Fq 'appliance_acme_flow_begin_order_poll' \
    kernel/include/appliance_acme_flow.h || {
    echo "ITEM-508 inc19: заголовок потока не публикует опрос заказа" >&2
    exit 1
}
grep -Fq 'appliance_acme_flow_begin_order_poll' \
    kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc19: поток не реализует опрос заказа" >&2
    exit 1
}
item508_inc19_poll=$(
    awk '/^enum appliance_acme_flow_status appliance_acme_flow_begin_order_poll\(/ { inside = 1 }
         inside { print }
         inside && /^}/ { exit }' kernel/src/appliance_acme_flow.c
)
[[ -n $item508_inc19_poll ]] || {
    echo "ITEM-508 inc19: тело опроса заказа не найдено" >&2
    exit 1
}
for item508_inc19_shape in \
    'APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING' \
    'flow->order_url.text' \
    'false, "", 0U' \
    'appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE)'; do
    [[ $item508_inc19_poll == *"$item508_inc19_shape"* ]] || {
        echo "ITEM-508 inc19: опрос не содержит «$item508_inc19_shape»" >&2
        exit 1
    }
done
item508_inc19_result=$(
    awk '/^enum appliance_acme_flow_status appliance_acme_flow_order_result\(/ { inside = 1 }
         inside { print }
         inside && /^}/ { exit }' kernel/src/appliance_acme_flow.c
)
[[ $item508_inc19_result == *'appliance_acme_flow_url_valid(order_url, order_url_length)'* ]] || {
    echo "ITEM-508 inc19: order_result не проверяет URL заказа" >&2
    exit 1
}
[[ $item508_inc19_result == *'flow->order_url.length = (uint32_t)order_url_length;'* ]] || {
    echo "ITEM-508 inc19: order_result не сохраняет URL заказа" >&2
    exit 1
}

# 2. The host proof drives the poll.
for item508_inc19_case in \
    'an order poll before finalize refuses' \
    'a pending order is polled as POST-as-GET' \
    'the order poll carries the empty payload'; do
    grep -Fq -- "$item508_inc19_case" tests/appliance-acme-flow-host.c || {
        echo "ITEM-508 inc19: проба потока не проверяет «$item508_inc19_case»" >&2
        exit 1
    }
done

# 3. The driver: the order Location is required, a pending finalize starts the
#    poll, and a second pending answer is a refusal.
for item508_inc19_driver in \
    'ACME_ORDER_STAGE_FINALIZE_POLL' \
    'appliance_acme_flow_begin_order_poll(' \
    'the order reply carried no Location' \
    'acme_order_publish("finalize processing")' \
    'acme_order_failure("the finalized order is not valid yet")' \
    'acme_order_flow.order_url.text'; do
    grep -Fq -- "$item508_inc19_driver" \
        kernel/src/appliance_acme_order_qemu.c || {
        echo "ITEM-508 inc19: драйвер не содержит «$item508_inc19_driver»" >&2
        exit 1
    }
done

# 4. The stub names the order URL and answers the poll with the finalized
#    order; the host proof asks for both documents.
for item508_inc19_stub in \
    '"/acme/order/1"' \
    '"https://10.0.2.2:%d/acme/order/1" % self.port' \
    'finalized_order_body(self.port).encode("ascii")'; do
    grep -Fq -- "$item508_inc19_stub" \
        scripts/hfoc-acme-directory-stub.py || {
        echo "ITEM-508 inc19: заглушка не содержит «$item508_inc19_stub»" >&2
        exit 1
    }
done
grep -Fq 'stub_request POST /acme/order/1' \
    scripts/test-acme-directory-stub-host.sh || {
    echo "ITEM-508 inc19: хост-проба не опрашивает URL заказа" >&2
    exit 1
}
grep -Fq 'POST /acme/new-order: Location' \
    scripts/test-acme-directory-stub-host.sh || {
    echo "ITEM-508 inc19: хост-проба не проверяет Location заказа" >&2
    exit 1
}
grep -Fq '[ACME-ORDER] order ok state=CERTIFICATE_VERIFIED' \
    scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc19: прогон не требует проверенной цепочки" >&2
    exit 1
}

# 5. The documents name the increment and the CSR boundary it leaves.
grep -Fq 'ITEM-508 increment 19' docs/ROADMAP.md || {
    echo "ITEM-508 inc19: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 19' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc19: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
