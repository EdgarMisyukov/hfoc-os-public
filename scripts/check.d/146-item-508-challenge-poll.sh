#!/usr/bin/env bash
# ITEM-508 increment 18: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The ACME client waits for a `processing` challenge.
#
# A real CA rarely answers the first challenge POST with `valid`; RFC 8555
# section 7.5.1 polls the challenge URL with POST-as-GET until the challenge
# settles.  This slice pins that the poll is a real POST-as-GET built by the
# flow, that the fixture driver takes the poll path, and that the offline stub
# exercises it (first answer `processing`, the poll `valid`) before the guest
# ever boots.
for item508_inc18_file in \
    kernel/include/appliance_acme_flow.h \
    kernel/src/appliance_acme_flow.c \
    kernel/src/appliance_acme_order_qemu.c \
    scripts/hfoc-acme-directory-stub.py \
    scripts/test-acme-directory-stub-host.sh \
    scripts/test-qemu-acme-order.sh \
    tests/appliance-acme-flow-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc18_file ]] || {
        echo "ITEM-508 inc18: отсутствует $item508_inc18_file" >&2
        exit 1
    }
done

# 1. The flow publishes the poll step and builds it as POST-as-GET: the empty
#    payload, the account's `kid`, and only while the challenge is pending.
grep -Fq 'appliance_acme_flow_begin_challenge_poll' \
    kernel/include/appliance_acme_flow.h || {
    echo "ITEM-508 inc18: заголовок потока не публикует опрос челленджа" >&2
    exit 1
}
grep -Fq 'appliance_acme_flow_begin_challenge_poll' \
    kernel/src/appliance_acme_flow.c || {
    echo "ITEM-508 inc18: поток не реализует опрос челленджа" >&2
    exit 1
}
item508_inc18_poll=$(
    awk '/^enum appliance_acme_flow_status appliance_acme_flow_begin_challenge_poll\(/ { inside = 1 }
         inside { print }
         inside && /^}/ { exit }' kernel/src/appliance_acme_flow.c
)
[[ -n $item508_inc18_poll ]] || {
    echo "ITEM-508 inc18: тело опроса челленджа не найдено" >&2
    exit 1
}
for item508_inc18_shape in \
    'APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING' \
    'flow->authorization.http01.url.text' \
    'false, "", 0U' \
    'appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE)'; do
    [[ $item508_inc18_poll == *"$item508_inc18_shape"* ]] || {
        echo "ITEM-508 inc18: опрос не содержит «$item508_inc18_shape»" >&2
        exit 1
    }
done

# 2. The host proof drives the poll: the refusal outside the pending state,
#    the POST-as-GET request and its empty payload, and the `valid` result.
for item508_inc18_case in \
    'a poll before the challenge POST refuses' \
    'a processing challenge is polled as POST-as-GET' \
    'the challenge poll carries the empty payload'; do
    grep -Fq -- "$item508_inc18_case" tests/appliance-acme-flow-host.c || {
        echo "ITEM-508 inc18: проба потока не проверяет «$item508_inc18_case»" >&2
        exit 1
    }
done

# 3. The driver owns one poll stage: a pending challenge starts the poll, a
#    valid one opens finalize, and a second pending answer is a refusal.
for item508_inc18_driver in \
    'ACME_ORDER_STAGE_CHALLENGE_POLL' \
    'appliance_acme_flow_begin_challenge_poll(' \
    'acme_order_publish("challenge processing")' \
    'acme_order_failure("the challenge is not valid yet")'; do
    grep -Fq -- "$item508_inc18_driver" \
        kernel/src/appliance_acme_order_qemu.c || {
        echo "ITEM-508 inc18: драйвер не содержит «$item508_inc18_driver»" >&2
        exit 1
    }
done

# 4. The stub answers `processing` first and `valid` afterwards; the host
#    proof asks twice and the QEMU harness still waits for the verified chain.
for item508_inc18_stub in \
    'self.challenge_answers += 1' \
    '"processing" if self.challenge_answers == 1 else "valid"' \
    'challenge_body(self.port, status)'; do
    grep -Fq -- "$item508_inc18_stub" \
        scripts/hfoc-acme-directory-stub.py || {
        echo "ITEM-508 inc18: заглушка не содержит «$item508_inc18_stub»" >&2
        exit 1
    }
done
item508_inc18_requests=$(
    grep -c 'stub_request POST /acme/chall/1' \
        scripts/test-acme-directory-stub-host.sh
)
[[ $item508_inc18_requests -eq 2 ]] || {
    echo "ITEM-508 inc18: хост-проба не делает первый запрос и опрос" >&2
    exit 1
}
grep -Fq 'expected_challenge_poll' scripts/test-acme-directory-stub-host.sh || {
    echo "ITEM-508 inc18: хост-проба не различает processing и valid" >&2
    exit 1
}
grep -Fq '"status":"processing","token":"tok3n-ABC_123"' \
    scripts/test-acme-directory-stub-host.sh || {
    echo "ITEM-508 inc18: первый ответ челленджа не processing" >&2
    exit 1
}
grep -Fq '[ACME-ORDER] order ok state=CERTIFICATE_VERIFIED' \
    scripts/test-qemu-acme-order.sh || {
    echo "ITEM-508 inc18: прогон не требует проверенной цепочки" >&2
    exit 1
}

# 5. The documents name the increment and the order-poll boundary it leaves.
grep -Fq 'ITEM-508 increment 18' docs/ROADMAP.md || {
    echo "ITEM-508 inc18: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-508 increment 18' docs/ARCHITECTURE.md || {
    echo "ITEM-508 inc18: ARCHITECTURE не называет инкремент" >&2
    exit 1
}
