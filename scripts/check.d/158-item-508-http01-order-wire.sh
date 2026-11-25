#!/usr/bin/env bash
# ITEM-508 increment 31: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal Makefile and shell text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The order proof arms the HTTP-01 listener from the flow it is driving.
#
# Increment 29 proved the listener on the wire with a fixture literal token and
# increment 30 added the production connector that arms it from the order flow,
# but only on the host.  This increment joins the two proofs: the order fixture
# kernel arms the real listener through `appliance_production_acme_http01_arm_flow()`
# when the flow has parsed its `http-01` challenge, and the host fetches the
# answer from guest port 80 with a real HTTP/1.1 client while the order runs.
# The pins below are: one fixture epoch named once for both fixture families
# (no SNTP peer exists in either proof, and the decision module refuses a zero
# clock by name), the listener handed to the order fixture, the driver arming
# only through the connector and disarming on the listener's own served
# counter, and the host comparison of the body byte for byte.
for item508_inc31_file in \
    kernel/include/appliance_acme_fixture_clock_qemu.h \
    kernel/src/appliance_acme_fixture_clock_qemu.c \
    kernel/src/appliance_acme_order_qemu.c \
    kernel/src/appliance_acme_http01_fixture_qemu.c \
    kernel/src/kernel.c \
    scripts/test-qemu-acme-order.sh \
    Makefile \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc31_file ]] || {
        echo "ITEM-508 inc31: отсутствует $item508_inc31_file" >&2
        exit 1
    }
done

# 1. One fixture epoch, named once for both fixture families.  The literal must
#    appear exactly once in the kernel tree: a second copy is how the two
#    proofs would drift apart.
for item508_inc31_clock_pin in \
    '#define APPLIANCE_ACME_FIXTURE_EPOCH UINT32_C(1800000000)' \
    'uint32_t appliance_acme_fixture_epoch_seconds(void)' \
    'return APPLIANCE_ACME_FIXTURE_EPOCH;' \
    '#if defined(HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST) || \' \
    'defined(HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST)'; do
    grep -Fq -- "$item508_inc31_clock_pin" \
        kernel/src/appliance_acme_fixture_clock_qemu.c || {
        echo "ITEM-508 inc31: общие часы не содержат «$item508_inc31_clock_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'uint32_t appliance_acme_fixture_epoch_seconds(void);' \
    kernel/include/appliance_acme_fixture_clock_qemu.h || {
    echo "ITEM-508 inc31: заголовок общих часов не объявляет эпоху" >&2
    exit 1
}
item508_inc31_epoch_literal=$(grep -rF 'UINT32_C(1800000000)' kernel/ | wc -l)
[[ $item508_inc31_epoch_literal -eq 1 ]] || {
    echo "ITEM-508 inc31: фикстурная эпоха названа $item508_inc31_epoch_literal раз" \
        >&2
    exit 1
}

# 2. Both fixture families read that one epoch for the listener's poll, the
#    order driver gets the production listener, and the production branch keeps
#    the validated clock unchanged.
for item508_inc31_kernel_pin in \
    'appliance_acme_fixture_epoch_seconds()' \
    'appliance_acme_order_qemu_test_tick(' \
    '&production_acme_http01, hfoc_lwip_time_now()'; do
    grep -Fq -- "$item508_inc31_kernel_pin" kernel/src/kernel.c || {
        echo "ITEM-508 inc31: kernel.c не содержит «$item508_inc31_kernel_pin»" \
            >&2
        exit 1
    }
done
python3 - <<'PY' || exit 1
import pathlib
import re

text = pathlib.Path("kernel/src/kernel.c").read_text()
guard = "#if defined(HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST) ||"
block = None
for match in re.finditer(re.escape(guard), text):
    position = match.start()
    else_at = text.find("#else", position)
    end = text.find("#endif", else_at if else_at != -1 else position)
    if else_at == -1 or end == -1:
        continue
    candidate = text[position:else_at]
    if "appliance_production_acme_http01_poll(" in candidate:
        block = (candidate, text[else_at:end])
        break
if block is None:
    raise SystemExit("ITEM-508 inc31: ветвь общих фикстурных часов не найдена")
fixture, production = block
if fixture.count("appliance_acme_fixture_epoch_seconds()") != 1:
    raise SystemExit("ITEM-508 inc31: часы фикстур названы не ровно один раз")
poll = fixture.find("appliance_production_acme_http01_poll(")
if poll == -1 or "fixture_now_seconds" not in fixture[poll:fixture.find(";", poll)]:
    raise SystemExit("ITEM-508 inc31: poll слушателя получает не часы фикстур")
if re.search(
        r"appliance_production_acme_http01_poll\(\s*&production_acme_http01,\s*"
        r"appliance_production_network_time_epoch_seconds\(\s*service_now_ms\)\);",
        production) is None:
    raise SystemExit("ITEM-508 inc31: production-ветвь потеряла проверенные часы")
if "#ifdef HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST" not in fixture or \
        "appliance_acme_http01_fixture_qemu_tick(" not in fixture:
    raise SystemExit("ITEM-508 inc31: фикстурный тик HTTP-01 потерян")
PY

# 3. The order driver arms only through the connector, only once the production
#    listener is listening, and disarms on the listener's own served counter so
#    the host fetch cannot miss the armed window.  It carries no token literal
#    and no second challenge decision.
for item508_inc31_driver_pin in \
    'appliance_production_acme_http01_arm_flow(' \
    'appliance_production_acme_http01_listening(acme_order_listener)' \
    'appliance_production_acme_http01_served_count(' \
    'appliance_production_acme_http01_disarm(' \
    'appliance_acme_fixture_epoch_seconds()' \
    'static void acme_order_arm_listener(void)' \
    'static void acme_order_disarm_served_listener(void)' \
    '"[ACME-ORDER] challenge "' \
    'acme_order_publish_listener("armed=1")' \
    'acme_order_publish_listener("armed=0 served=1")'; do
    grep -Fq -- "$item508_inc31_driver_pin" \
        kernel/src/appliance_acme_order_qemu.c || {
        echo "ITEM-508 inc31: драйвер заказа не содержит «$item508_inc31_driver_pin»" \
            >&2
        exit 1
    }
done
for item508_inc31_driver_forbidden in \
    'appliance_acme_challenge_arm_token(' \
    'tok3n-ABC_123'; do
    if grep -Fq -- "$item508_inc31_driver_forbidden" \
        kernel/src/appliance_acme_order_qemu.c; then
        echo "ITEM-508 inc31: драйвер дублирует «$item508_inc31_driver_forbidden»" \
            >&2
        exit 1
    fi
done
python3 - <<'PY' || exit 1
import pathlib

source = pathlib.Path("kernel/src/appliance_acme_order_qemu.c").read_text()
arm = source.find("acme_order_arm_listener();")
authorization = source.find('acme_order_publish("authorization");')
if arm == -1 or authorization == -1 or arm < authorization:
    raise SystemExit(
        "ITEM-508 inc31: вооружение не следует за разобранной авторизацией")
PY

# 4. The host fetches the armed answer from guest port 80 on its own port and
#    compares the body byte for byte with `token.thumbprint` read from the
#    driver's public record; the token is disarmed only after it was served.
for item508_inc31_script_pin in \
    'HTTP01_PORT=${HFOC_ACME_ORDER_QEMU_HTTP01_PORT:-18448}' \
    'hostfwd=tcp:127.0.0.1:$HTTP01_PORT-:80' \
    'python3 - "$PORT" "$HTTP01_PORT"' \
    "grep -Fq '[ACME-ORDER] challenge armed=1'" \
    "grep -Fq '[ACME-ORDER] challenge armed=0 served=1'" \
    "grep -Fq '[ACME-HTTP01-FIXTURE]'" \
    'token = "tok3n-ABC_123"' \
    'answer = ("%s.%s" % (token, thumbprint)).encode("ascii")' \
    'if body != answer:' \
    "grep -F '[ACME-HTTP01] ' \"\$(serial_view \"\$SERIAL\")\" | grep -Fq 'served=1'" \
    'HFOC-QEMU-MARKER-TIMEOUT' \
    '*** HFOC KERNEL PANIC ***'; do
    grep -Fq -- "$item508_inc31_script_pin" scripts/test-qemu-acme-order.sh || {
        echo "ITEM-508 inc31: прогон заказа не содержит «$item508_inc31_script_pin»" \
            >&2
        exit 1
    }
done
if grep -Fq 'in body' scripts/test-qemu-acme-order.sh; then
    echo "ITEM-508 inc31: прогон сравнивает тело подстрокой" >&2
    exit 1
fi

# 5. Both fixture families link the shared clock object, the order target pins
#    the HTTP-01 host port, and the production kernel and ISO still carry no
#    order record.
for item508_inc31_make_pin in \
    '$(ACME_ORDER_OBJ_DIR)/appliance_acme_fixture_clock_qemu.o' \
    '$(ACME_HTTP01_OBJ_DIR)/appliance_acme_fixture_clock_qemu.o' \
    'HFOC_ACME_ORDER_QEMU_HTTP01_PORT='"'"'18448'"'"' \' \
    "if strings \$(KERNEL) | grep -Fq '[ACME-ORDER]'" \
    "if strings \$(IMAGE) | grep -Fq '[ACME-ORDER]'"; do
    grep -Fq -- "$item508_inc31_make_pin" Makefile || {
        echo "ITEM-508 inc31: Makefile не содержит «$item508_inc31_make_pin»" >&2
        exit 1
    }
done

# 6. The documents name the increment.
for item508_inc31_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-508 increment 31' "$item508_inc31_doc" || {
        echo "ITEM-508 inc31: $item508_inc31_doc не называет инкремент" >&2
        exit 1
    }
done
