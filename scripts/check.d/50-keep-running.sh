#!/usr/bin/env bash
# ITEM-428: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The appliance keeps running and holds the address it was given.
#
# The 2026-09-16 hardware run's own boot log, read from the acceptance disk,
# ends ten seconds after the installed system booted on
# `[PRODUCTION-DHCP] state=ADDRESSING lease=NONE attempt=11/40`; every record
# that lives in the main loop stops with it, and the machine answered neither
# ARP nor ICMP for the address it was reserved afterwards.  The run could not
# say whether the appliance had never transmitted, had transmitted and been
# ignored, or had been answered and dropped the answer, and the code could not
# say whether the loop was alive with nothing to report or stopped.
#
# Four things are pinned here, because each of them is a claim the next
# hardware run depends on:
#
#   * the live backend enables PCI memory space and bus mastering before it
#     asks the part to touch a descriptor - the run's Realtek NIC answered
#     every MMIO register read while its transmit ring never returned one
#     descriptor, which is exactly what a function without bus mastering
#     looks like - and the command register it read back is published;
#   * the addressing record carries the wire view (discover/request/offer/ack/
#     refused), so "no lease" is never again three causes at once;
#   * the main loop publishes a periodic heartbeat carrying the live
#     frame-path counters, so a machine that is up always publishes and a
#     machine that is not can be read as one;
#   * a DHCP frame the transmit path refuses is published as the reason the
#     cycle is parked and the cycle keeps its bounded cadence, instead of the
#     silent park that ends the run's log.
for keep_running_file in \
    kernel/include/appliance_loop_heartbeat.h \
    kernel/src/appliance_loop_heartbeat.c \
    tests/loop-heartbeat-host.c \
    tests/rtl8168-host.c \
    tests/appliance-network-wire-host.c; do
    [[ -f $keep_running_file ]] || {
        echo "ITEM-428: отсутствует $keep_running_file" >&2
        exit 1
    }
done

# The bus-master enable is the fix, not a comment: the helper writes both bits
# and the Realtek probe calls it before the part is brought up, and the value
# it reads back is published on the network record.
for keep_running_bus_master in \
    'APN_PCI_COMMAND_MEMORY_SPACE UINT32_C(0x0002)' \
    'APN_PCI_COMMAND_BUS_MASTER UINT32_C(0x0004)' \
    'apn_pci_enable_bus_master(' \
    'appliance_production_network_boot_pci_command('; do
    grep -Fq -- "$keep_running_bus_master" \
        kernel/src/appliance_production_network_x86.c || {
        echo "ITEM-428: драйвер NIC не включает bus mastering: $keep_running_bus_master" >&2
        exit 1
    }
done
grep -Fq 'PCI_BUS_MASTER_REFUSED' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-428: отказ включить bus mastering не публикуется" >&2
    exit 1
}

# The wire view and the frame-path counters are rendered by the boot records,
# and the heartbeat is a flush trigger so it reaches the appliance's own disk.
for keep_running_record in \
    '" discover="' \
    '" request="' \
    '" offer="' \
    '" ack="' \
    '" refused="' \
    '" pci_cmd=0x"' \
    '"[PRODUCTION-LOOP] state=RUNNING ticks="' \
    '" tx_sub="' \
    '" tx_recl="' \
    '" tx_lost="' \
    '" tx_full="' \
    '" tx_rst="' \
    '" rx_ok="' \
    '" rx_err="'; do
    grep -Fq -- "$keep_running_record" kernel/src/kernel.c || {
        echo "ITEM-428: запись не несёт поле: $keep_running_record" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS 10000U' \
    kernel/include/appliance_loop_heartbeat.h || {
    echo "ITEM-428: интервал heartbeat изменился без пересмотра бюджета журнала" >&2
    exit 1
}
grep -Fq '"[PRODUCTION-LOOP]",' kernel/src/appliance_boot_log.c || {
    echo "ITEM-428: heartbeat не доходит до журнала на диске" >&2
    exit 1
}
grep -Fq 'log_production_loop_heartbeat();' kernel/src/kernel.c || {
    echo "ITEM-428: главный цикл не публикует heartbeat" >&2
    exit 1
}
grep -Fq 'TEST_DESCRIPTOR_COUNT - 2U' tests/rtl8168-host.c || {
    echo "ITEM-428: host-проба не считает брошенные кадры" >&2
    exit 1
}

# A refused DHCP frame is published and the cycle keeps moving.
for keep_running_refusal in \
    'appliance_network_lifecycle_dhcp_transport_refused(' \
    '++wire->dhcp_send_refusals;' \
    '++wire->dhcp_discovers_sent;' \
    '++wire->dhcp_offers_received;' \
    '++wire->dhcp_acks_received;'; do
    grep -Fq -- "$keep_running_refusal" \
        kernel/src/appliance_network_wire.c || {
        echo "ITEM-428: провод не фиксирует отказ: $keep_running_refusal" >&2
        exit 1
    }
done
grep -Fq 'appliance_network_lifecycle_dhcp_transport_refused(' \
    kernel/src/appliance_network_lifecycle.c || {
    echo "ITEM-428: жизненный цикл не публикует отказ передачи" >&2
    exit 1
}
grep -Fq 'appliance_network_lifecycle_dhcp_transport_refused(' \
    kernel/include/appliance_network_lifecycle.h || {
    echo "ITEM-428: отказ передачи не объявлен в заголовке" >&2
    exit 1
}

# The driver keeps the ring's own liveness and can hand a stalled ring back.
for keep_running_ring in \
    'enum rtl8168_status rtl8168_tx_rewind(' \
    'ring->submitted' \
    'ring->reclaimed' \
    'ring->abandoned'; do
    grep -Fq -- "$keep_running_ring" kernel/src/rtl8168.c || {
        echo "ITEM-428: кольцо передачи не ведёт счёт: $keep_running_ring" >&2
        exit 1
    }
done
grep -Fq 'rtl8168_tx_rewind(' kernel/include/rtl8168.h || {
    echo "ITEM-428: rtl8168_tx_rewind не объявлен" >&2
    exit 1
}

# The heartbeat cadence is proven on the host by the suite itself: this slice
# pins the target, the serial recipe and the manifest, so the proof cannot be
# dropped from the suite the CI phase runs.
grep -Fq 'test-loop-heartbeat-host:' Makefile || {
    echo "Makefile не содержит test-loop-heartbeat-host" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the Makefile says
grep -Fq '$(MAKE) test-loop-heartbeat-host' Makefile || {
    echo "test-host-suite-serial не вызывает test-loop-heartbeat-host" >&2
    exit 1
}
grep -Fq 'test-loop-heartbeat-host' tests/test-host-suite-manifest.sh || {
    echo "манифест host suite не содержит test-loop-heartbeat-host" >&2
    exit 1
}

# The QEMU proof requires both the heartbeat and the wire view.
for keep_running_qemu in \
    'heartbeat_lines' \
    '"[PRODUCTION-LOOP] state=RUNNING" in line' \
    'discover=([0-9]+) request=([0-9]+) offer=([0-9]+)' \
    'ack=([0-9]+) refused=([0-9]+)' \
    'pci_cmd=0x'; do
    grep -Fq -- "$keep_running_qemu" \
        scripts/test-qemu-production-dhcp-dns.sh || {
        echo "ITEM-428: QEMU-проба не требует: $keep_running_qemu" >&2
        exit 1
    }
done

# The rule is documented where a maintainer looks for it.
for keep_running_doc in \
    'ITEM-428' \
    'bus mastering'; do
    grep -Fq -- "$keep_running_doc" docs/hfoc-hardware-acceptance.md || {
        echo "docs/hfoc-hardware-acceptance.md не фиксирует ITEM-428: $keep_running_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-428' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-428" >&2
    exit 1
}

# The enable is a call, not a helper nobody uses: the live backend reaches it
# before it programs a descriptor.  The helper's own definition contains its
# name, so both call sites are pinned by the register each path publishes.
for keep_running_callsite in \
    '&apn.rtl.pci_command' \
    '&apn.virtio_pci_command'; do
    grep -Fq -- "$keep_running_callsite" \
        kernel/src/appliance_production_network_x86.c || {
        echo "ITEM-428: вызов включения bus mastering потерян:" \
            "$keep_running_callsite" >&2
        exit 1
    }
done
# A register nobody read is not a value: the virtio path publishes the command
# register only after the enable helper ran, and the frame counters only after
# the bind completed.  The guard itself is pinned, not only the flag's name:
# the flag is set on the failure path too (the register WAS read), so a check
# that matched the identifier alone would pass with the guard deleted.
for keep_running_valid in \
    'apn.virtio_pci_command_valid = true;' \
    'if (!apn.virtio_pci_command_valid) {' \
    'apn.virtio_bound = true;' \
    'counters->available = apn.virtio_bound;'; do
    grep -Fq -- "$keep_running_valid" \
        kernel/src/appliance_production_network_x86.c || {
        echo "ITEM-428: непрочитанный регистр публикуется как значение:" \
            "$keep_running_valid" >&2
        exit 1
    }
done
grep -Fq 'apn.virtio_reason = "PCI_BUS_MASTER_REFUSED";' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-428: отказ virtio по bus mastering не назван" >&2
    exit 1
}
grep -Fq 'return apn.virtio_reason;' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-428: причина отказа virtio не публикуется" >&2
    exit 1
}

# The transmit restart stops the part before it re-arms the rings, and it
# counts the attempt before it touches the part.  Both are the difference
# between a recovery and a second failure: a rewind of a live device asks it to
# fetch the stale descriptors the ring is full of - and can catch one with its
# address already cleared - while an uncounted attempt turns a part that cannot
# come up into a reset on every turn of the main loop.  The order is read out
# of the function body itself, so moving a call back is caught here.
keep_running_restart_order=$(awk '
    /^static bool apn_rtl_tx_restart\(/ { inside = 1; next }
    inside && /^\}/ { exit }
    inside && /rtl8168_reset\(&apn\.rtl\.io\)/ { if (!reset_at) reset_at = NR }
    inside && /rtl8168_tx_rewind\(/ { if (!rewind_at) rewind_at = NR }
    inside && /rtl8168_rx_post\(/ { if (!rx_post_at) rx_post_at = NR }
    inside && /apn_rtl_bring_up\(\)/ { if (!bring_up_at) bring_up_at = NR }
    inside && /\+\+apn\.rtl\.tx_restarts;/ { if (!attempt_at) attempt_at = NR }
    END {
        if (!reset_at || !rewind_at || !rx_post_at || !bring_up_at ||
            !attempt_at) {
            exit 1
        }
        if (!(attempt_at < reset_at && reset_at < rewind_at &&
              rewind_at < rx_post_at && rx_post_at < bring_up_at)) {
            exit 1
        }
        printf "ordered"
    }
' kernel/src/appliance_production_network_x86.c) || {
    echo "ITEM-428: порядок перезапуска передачи не читается" >&2
    exit 1
}
[[ $keep_running_restart_order == ordered ]] || {
    echo "ITEM-428: перезапуск не останавливает устройство до возврата кольца" \
        "или не считает попытку до сброса" >&2
    exit 1
}
grep -Fq 'apn.rtl.tx_stall_armed = false;' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-428: исчерпанный бюджет перезапусков не разоружает stall" >&2
    exit 1
}
# The disarm that matters is the one on the exhausted path: the stall flags are
# cleared on every attempt as well, so the bare line above would pass with the
# exhausted branch left armed - and an armed stall with no attempts left
# re-decides the refusal on every turn of the loop.
keep_running_exhausted=$(awk '
    /apn\.rtl\.reason = "TX_RING_FULL_EXHAUSTED";/ { found = 1; next }
    found && /tx_stall_armed = false;/ { print "disarmed"; exit }
    found && /^    }/ { exit }
' kernel/src/appliance_production_network_x86.c)
[[ $keep_running_exhausted == disarmed ]] || {
    echo "ITEM-428: исчерпанный бюджет оставляет stall вооружённым" >&2
    exit 1
}

# The reason the backend publishes is part of the network record, so a change
# of reason republishes the line: a stated refusal that is never printed is not
# a stated refusal.
for keep_running_reason in \
    'backend_reason == last_backend_reason' \
    'last_backend_reason = backend_reason;'; do
    grep -Fq -- "$keep_running_reason" kernel/src/kernel.c || {
        echo "ITEM-428: [PRODUCTION-NET] не переиздаётся по смене reason:" \
            "$keep_running_reason" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------------
# ITEM-439: a receive path that is not armed is not a bound receive path.
#
# The 2026-09-17 bisect read `rx_ok=0 rx_err=0 offer=0` from a medium built
# before ITEM-435's RXDV-gate fix, and its first suspect (ITEM-416) never
# touched the driver.  What is left on main is this: `apn_rtl_bind()` sets
# `readiness.receive_path_ready = true` without asking, while
# `apn_rtl_bring_up()` already reads `rtl8168_rx_armed()` back from the part.
# A receiver that never held the ring could therefore bind as READY, and the
# only trace was an all-zero receive line.
for receive_path_ready in \
    'bool rx_ready_known;' \
    'apn.rtl.rx_ready_known = true;' \
    'if (!apn.rtl.rx_ready_known) {' \
    'apn.rtl.reason = "RX_ARM_PROBE_FAILED";' \
    'if (!apn.rtl.rx_ready) {' \
    'apn.rtl.rx_ready_known = false;' \
    'apn.rtl.reason = "RX_NOT_ARMED";'; do
    grep -Fq -- "$receive_path_ready" \
        kernel/src/appliance_production_network_x86.c || {
        echo "ITEM-439: привязка не проверяет вооружённость приёма:" \
            "$receive_path_ready" >&2
        exit 1
    }
done
if grep -Fq 'so both paths are honestly ready' \
    kernel/src/appliance_production_network_x86.c; then
    echo "ITEM-439: приём всё ещё объявляется готовым без ответа железа" >&2
    exit 1
fi

# The QEMU proof boots with no configuration and answers DHCP, so its own
# heartbeat has to say the ring was armed and frames arrived; `rx_ok=0` alone
# is the shape the bisect could not interpret.
for receive_path_proof in \
    'rx_drop=[0-9]+.*rx_err=[0-9]+.*rx_arm=[0-9]+' \
    'the receive ring is not armed on a machine that answered' \
    'the receive path accepted and refused no frame at all on a'; do
    grep -Fq -- "$receive_path_proof" \
        scripts/test-qemu-production-dhcp-dns.sh || {
        echo "ITEM-439: QEMU-проба не требует вооружённого приёма:" \
            "$receive_path_proof" >&2
        exit 1
    }
done

# The host proof drives the arming read the bind now depends on, with the
# negative control (a ring address the part was not given) and the IO error
# that is not an answer at all.
for receive_path_host in \
    'rtl8168_rx_armed(&io, TEST_DMA_BASE, &armed)' \
    'the receiver holds the ring the bring-up programmed' \
    'a ring address the part was not given reads as not armed' \
    'an unreadable part is an error, not an armed or disarmed answer'; do
    grep -Fq -- "$receive_path_host" tests/rtl8168-host.c || {
        echo "ITEM-439: host-проба не фиксирует вооружённость приёма:" \
            "$receive_path_host" >&2
        exit 1
    }
done
