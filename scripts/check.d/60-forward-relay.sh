#!/usr/bin/env bash
# ITEM-449: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The traffic to the declared server goes through the machine.
#
# Step 1 pinned the declaration and the access rule.  This slice pins the relay
# that acts on them, and the claims that make it safe:
#
#   * the listener is the second one the profile has ever had, and the profile
#     says so: two listen PCBs and enough connection PCBs for one relay session
#     beside the management plane - the item's whole change to the frozen
#     lwIP options;
#   * the admission decision happens before any outbound connection exists: the
#     source is checked by `forward_config_admit()` and a denied client is
#     aborted without `tcp_connect` ever being called for it;
#   * the relay is a TCP port forwarder, not IP forwarding: `IP_FORWARD` and
#     `IP_FORWARD_ALLOW_TX_ON_RX_NETIF` stay off and no NAT module is
#     introduced, because the appliance has one netif and `ip4_forward()`
#     refuses to send back out of the receiving netif;
#   * both directions are bounded and backpressured: a received chain is copied
#     whole or refused whole (never acknowledged twice), and `tcp_recved` is
#     called only for bytes the relay holds;
#   * a session that stopped moving is ended by the idle deadline, and every
#     ending is published with its verdict and counters;
#   * the byte-level QEMU proof installs the forward medium, exchanges a
#     challenge through the relay, and proves the refusal for a source the
#     declaration does not name - with the target never dialled.
for forward_relay_file in \
    kernel/include/hfoc_lwip_forward.h \
    kernel/src/hfoc_lwip_forward.c \
    kernel/include/lwipopts.h \
    kernel/src/kernel.c \
    scripts/test-qemu-production-forward.sh \
    docs/hfoc-production-forward-declaration.md; do
    [[ -f $forward_relay_file ]] || {
        echo "ITEM-449: отсутствует $forward_relay_file" >&2
        exit 1
    }
done

# The relay owner publishes the listener, the session and the counters.
for forward_relay_symbol in \
    hfoc_lwip_forward_init \
    hfoc_lwip_forward_listen \
    hfoc_lwip_forward_service \
    hfoc_lwip_forward_shutdown \
    hfoc_lwip_forward_take_event \
    HFOC_LWIP_FORWARD_EVENT_LISTENING \
    HFOC_LWIP_FORWARD_EVENT_REFUSED_SOURCE \
    HFOC_LWIP_FORWARD_EVENT_SESSION_OK \
    HFOC_LWIP_FORWARD_EVENT_SESSION_FAILED \
    HFOC_LWIP_FORWARD_BUFFER_MAX \
    HFOC_LWIP_FORWARD_IDLE_MS; do
    grep -Fq "$forward_relay_symbol" \
        kernel/include/hfoc_lwip_forward.h || {
        echo "ITEM-449: релей не публикует $forward_relay_symbol" >&2
        exit 1
    }
done
# The access rule is enforced before the outbound connection exists.
grep -Fq 'forward_config_admit(&forward->config, source)' \
    kernel/src/hfoc_lwip_forward.c || {
    echo "ITEM-449: релей не спрашивает правило доступа" >&2
    exit 1
}
if ! awk '/forward_config_admit/{admit=NR} /tcp_connect\(/{connect=NR}
          END{exit !(admit != 0 && connect != 0 && admit < connect)}' \
        kernel/src/hfoc_lwip_forward.c; then
    echo "ITEM-449: исходящее соединение создаётся раньше допуска" >&2
    exit 1
fi
# A denied source is aborted, never dialled.
grep -Fq 'SOURCE_NOT_ALLOWED' kernel/src/hfoc_lwip_forward.c || {
    echo "ITEM-449: отказ источника не публикуется" >&2
    exit 1
}
# Both directions are bounded, and a refused chain is neither acked nor freed.
grep -Fq 'p->tot_len > HFOC_LWIP_FORWARD_BUFFER_MAX - used' \
    kernel/src/hfoc_lwip_forward.c || {
    echo "ITEM-449: у релея нет границы буфера" >&2
    exit 1
}
grep -Fq 'return ERR_MEM;' kernel/src/hfoc_lwip_forward.c || {
    echo "ITEM-449: релей не отказывает цепочке, которая не помещается" >&2
    exit 1
}
grep -Fq 'tcp_recved(pcb, p->tot_len)' kernel/src/hfoc_lwip_forward.c || {
    echo "ITEM-449: релей не подтверждает только удержанные байты" >&2
    exit 1
}
grep -Fq 'HFOC_LWIP_FORWARD_IDLE_MS' kernel/src/hfoc_lwip_forward.c || {
    echo "ITEM-449: у сессии нет дедлайна простоя" >&2
    exit 1
}

# ITEM-644: the relay must not confuse its own tcp_abort() with a PCB the
# stack removed on its own (a backend that exhausted TCP_SYNMAXRTX).  The
# stack-originated ERR_ABRT has to fail the surviving leg, publish a verdict
# and leave the one slot usable for the next client.
for forward_error_symbol in aborting_target aborting_client; do
    grep -Fq "$forward_error_symbol" kernel/include/hfoc_lwip_forward.h || {
        echo "ITEM-644: нет признака собственного abort $forward_error_symbol" >&2
        exit 1
    }
done
for forward_error_verdict in TARGET_ABORTED CLIENT_ABORTED; do
    grep -Fq "$forward_error_verdict" kernel/src/hfoc_lwip_forward.c || {
        echo "ITEM-644: нет вердикта $forward_error_verdict" >&2
        exit 1
    }
done
grep -Fq 'forward->client_pcb != NULL || forward->target_pcb != NULL' \
    kernel/src/hfoc_lwip_forward.c || {
    echo "ITEM-644: дедлайн простоя не покрывает одиночную сторону" >&2
    exit 1
}
[[ -f tests/lwip-forward-host.c ]] || {
    echo "ITEM-644: нет host-регрессии отказного пути relay" >&2
    exit 1
}
grep -Fq 'test-lwip-forward-host' Makefile || {
    echo "ITEM-644: host-регрессия не зарегистрирована" >&2
    exit 1
}
# No IP forwarding and no NAT: this is a port forwarder on one netif.
if grep -nE '(^|[^_A-Za-z])IP_FORWARD([^_A-Za-z]|$)|IP_NAPT|LWIP_NAT|ip4_forward' \
        kernel/include/lwipopts.h kernel/src/hfoc_lwip_forward.c; then
    echo "ITEM-449: релей включил IP-форвардинг вместо порта" >&2
    exit 1
fi
# The relay writes no device and no console: the caller renders the record.
if grep -nE '\b(serial_|console_)\w*' kernel/src/hfoc_lwip_forward.c; then
    echo "ITEM-449: релей пишет в консоль вместо записи" >&2
    exit 1
fi

# The profile carries exactly the item's change and states why.
grep -Fq '#define MEMP_NUM_TCP_PCB_LISTEN 3' kernel/include/lwipopts.h || {
    echo "ITEM-449: у профиля нет второго listen-PCB" >&2
    exit 1
}
grep -Fq '#define MEMP_NUM_TCP_PCB 7' kernel/include/lwipopts.h || {
    echo "ITEM-449: у профиля нет PCBs на сессию релея" >&2
    exit 1
}
grep -Fq 'ITEM-449' kernel/include/lwipopts.h || {
    echo "ITEM-449: изменение профиля не объяснено" >&2
    exit 1
}

# The kernel creates the owner from the declared rule and serves it in the loop.
grep -Fq 'hfoc_lwip_forward_init(' kernel/src/kernel.c || {
    echo "ITEM-449: владелец релея не создаётся" >&2
    exit 1
}
grep -Fq 'hfoc_lwip_forward_service(' kernel/src/kernel.c || {
    echo "ITEM-449: релей не обслуживается в главном цикле" >&2
    exit 1
}
for forward_relay_record in \
    '"[PRODUCTION-FORWARD] state=LISTENING "' \
    '"[PRODUCTION-FORWARD] state=SESSION verdict="' \
    '"[PRODUCTION-FORWARD] state=REFUSED "' \
    '"reason=SOURCE_NOT_ALLOWED src="' \
    '"[PRODUCTION-FORWARD] state=REFUSED reason=BUSY"' \
    '"UNAVAILABLE reason=LISTEN_FAILED detail="'; do
    grep -Fq "$forward_relay_record" kernel/src/kernel.c || {
        echo "ITEM-449: нет записи релея: $forward_relay_record" >&2
        exit 1
    }
done

# The byte-level proof does the exchange and proves the refusal.
for forward_relay_proof in \
    'HFOC-FORWARD-CLIENT-HELLO' \
    'HFOC-FORWARD-TARGET-ANSWER' \
    'state=SESSION verdict=OK' \
    'bytes_to_target' \
    'bytes_to_client' \
    'SOURCE_NOT_ALLOWED src=' \
    'the target was dialled for a refused source'; do
    grep -Fq "$forward_relay_proof" \
        scripts/test-qemu-production-forward.sh || {
        echo "ITEM-449: QEMU-проба не утверждает: $forward_relay_proof" >&2
        exit 1
    }
done
grep -Fq 'test-production-forward-qemu' Makefile || {
    echo "ITEM-449: проба релея не зарегистрирована" >&2
    exit 1
}
grep -Fq 'production-installer-forward-denied-media' Makefile || {
    echo "ITEM-449: medium с запрещающим списком не собирается" >&2
    exit 1
}
# The design note no longer promises the relay: it describes it.
grep -Fq 'One session is the budget' \
    docs/hfoc-production-forward-declaration.md || {
    echo "ITEM-449: заметка не описывает бюджет сессии" >&2
    exit 1
}
