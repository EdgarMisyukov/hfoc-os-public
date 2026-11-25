#!/usr/bin/env bash
# ITEM-494: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The second NIC vendor: Intel 8254x ("e1000"), proven without Intel hardware.
#
# ITEM-492 made the machine's controllers a property of the machine. This item
# answers the next question - can a second vendor be added without touching the
# first one - and this slice pins the rules that keep the answer honest:
#
#   * the claim is one part, not a vendor: `8086:100E` (82540EM) and nothing
#     else, so an Intel controller the driver was not written for still reads
#     UNSUPPORTED instead of being driven by a guess;
#   * the RTL8168 claim is untouched: adding a vendor does not renegotiate the
#     vendor that is already proven on real hardware;
#   * the driver masks interrupts and never enables them (the poller contract of
#     ITEM-389): no MSI, no APIC/IOAPIC/SMP, no xHCI - a second NIC vendor is a
#     driver, not an interrupt-architecture change (AGENTS.md section 3);
#   * the address on a bound record is the one the driver read, so `AVAILABLE`
#     carries `mac=` from the part and not a placeholder;
#   * the core is proven by a host proof registered in the canonical host suite,
#     because the acceptance host has no Intel controller: the QEMU/OVMF proof
#     is not physical hardware acceptance and this item must not claim it is.

for item494_file in \
    kernel/include/e1000.h \
    kernel/src/e1000.c \
    tests/e1000-host.c \
    docs/hfoc-nic-discovery.md; do
    test -f "$item494_file" || {
        echo "ITEM-494: отсутствует файл: $item494_file" >&2
        exit 1
    }
done

# --------------------------------------------------- one part, not a vendor --
grep -Fq 'NIC_BACKEND_NAME_E1000' kernel/src/nic_backends.c || {
    echo "ITEM-494: в списке backend'ов нет NIC_BACKEND_NAME_E1000" >&2
    exit 1
}
grep -Fq '(uint16_t)E1000_VENDOR_ID' kernel/src/nic_backends.c || {
    echo "ITEM-494: claim Intel не называет vendor id" >&2
    exit 1
}
grep -Fq '(uint16_t)E1000_DEVICE_ID_82540EM' kernel/src/nic_backends.c || {
    echo "ITEM-494: claim Intel не называет конкретную часть" >&2
    exit 1
}
# The claim names exactly one device id: the header defines one, and the claim
# table uses it.  A second id would need its own item and its own proof.
item494_ids=$(grep -c '^#define E1000_DEVICE_ID_' kernel/include/e1000.h)
[[ $item494_ids -eq 1 ]] || {
    echo "ITEM-494: драйвер объявляет $item494_ids идентификаторов устройств, а не один" >&2
    exit 1
}
# The first vendor is not renegotiated by the second one.
grep -Fq '(uint16_t)RTL8168_VENDOR_ID' kernel/src/nic_backends.c || {
    echo "ITEM-494: claim RTL8168 пропал при добавлении второго вендора" >&2
    exit 1
}

# ------------------------------------- a driver, not an interrupt change ------
for item494_forbidden in msi MSI apic APIC ioapic IOAPIC xhci XHCI smp SMP; do
    if grep -Fq -- "$item494_forbidden" kernel/src/e1000.c; then
        echo "ITEM-494: драйвер 8254x трогает $item494_forbidden" >&2
        exit 1
    fi
done
# Interrupts are masked, and the mask helper is the only writer of IMS/IMC.
grep -Fq 'E1000_REG_IMC' kernel/src/e1000.c || {
    echo "ITEM-494: драйвер 8254x не маскирует прерывания" >&2
    exit 1
}
grep -Fq 'E1000_REG_IMS, 0U' kernel/src/e1000.c || {
    echo "ITEM-494: драйвер 8254x не оставляет interrupt mask register пустым" >&2
    exit 1
}

# -------------------------------------- the address is the one the part gave --
grep -Fq 'e1000_mac_read' kernel/src/e1000.c || {
    echo "ITEM-494: драйвер 8254x не читает MAC из RAL/RAH" >&2
    exit 1
}
# The link is a PHY state: STATUS.LU only comes up after the negotiation
# completes, and the restart is what a machine whose peer appeared after the
# reset needs.
grep -Fq 'e1000_restart_autoneg' kernel/src/e1000.c || {
    echo "ITEM-494: драйвер 8254x не перезапускает авто-негоциацию PHY" >&2
    exit 1
}
grep -Fq 'E1000_MII_BMCR_ANRESTART' kernel/src/e1000.c || {
    echo "ITEM-494: перезапуск PHY не несёт ANRESTART" >&2
    exit 1
}
# The link bit position is part of the register map: STATUS.LU is bit 1
# (0x00000002).  Reading bit 2 made a machine whose link was up read down.
grep -Fq '#define E1000_STATUS_LU UINT32_C(1) << 1U' kernel/include/e1000.h || {
    echo "ITEM-494: STATUS.LU объявлен не битом 1" >&2
    exit 1
}
# Broadcast accept is RCTL.BAM (bit 15); SBP is "store bad packets" and
# accepting it for broadcast dropped every offer the peer sent.
grep -Fq '#define E1000_RCTL_BAM UINT32_C(1) << 15U' kernel/include/e1000.h || {
    echo "ITEM-494: RCTL.BAM объявлен не битом 15" >&2
    exit 1
}
grep -Fq 'E1000_RCTL_BAM' kernel/src/e1000.c || {
    echo "ITEM-494: bring-up не включает приём broadcast" >&2
    exit 1
}
grep -Fq 'e1000_restart_autoneg(&apn.e1000.io)' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-494: production-путь не перезапускает негоциацию PHY" >&2
    exit 1
}
grep -Fq 'nic_device_set_mac' kernel/src/nic_registry.c || {
    echo "ITEM-494: реестр NIC не принимает MAC от драйвера" >&2
    exit 1
}
grep -Fq 'apn_nic_record_bind_result(true, NULL, apn.rtl.mac)' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-494: связанная запись не несёт MAC, прочитанный драйвером" >&2
    exit 1
}
grep -Fq 'apn_nic_record_bind_result(true, NULL, apn.e1000.mac)' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-494: запись Intel при bind не несёт MAC, прочитанный драйвером" >&2
    exit 1
}

# ------------------------------------ the production path really binds Intel --
# The driver core alone is not the item: the production adapter must select the
# Intel backend, bind it to the same composition and drive its doorbells.
for item494_wiring in \
    'APN_BACKEND_E1000' \
    'apn_e1000_prepare' \
    'apn_e1000_bring_up' \
    'apn_e1000_bind' \
    'apn_e1000_transmit' \
    'apn_e1000_poll' \
    'E1000_REG_TDT' \
    'E1000_REG_RDT'; do
    grep -Fq -- "$item494_wiring" \
        kernel/src/appliance_production_network_x86.c || {
        echo "ITEM-494: production-адаптер не содержит $item494_wiring" >&2
        exit 1
    }
done
# And the poll is reached from the boot loop, not merely defined.
grep -Fq 'apn_e1000_poll(now_ms);' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-494: boot-петля не зовёт опрос Intel" >&2
    exit 1
}

# --------------------------------------------- the proof is registered, honestly --
grep -Fq 'test-e1000-host:' Makefile || {
    echo "ITEM-494: host-проба 8254x не зарегистрирована" >&2
    exit 1
}
# shellcheck disable=SC2016  # the pinned Makefile text is literal
grep -Fq '$(MAKE) test-e1000-host' Makefile || {
    echo "ITEM-494: host-проба 8254x не в каноническом host-suite" >&2
    exit 1
}
grep -Fq 'e1000 host tests: ALL OK' tests/e1000-host.c || {
    echo "ITEM-494: host-проба 8254x не печатает вердикт" >&2
    exit 1
}
# The main proof: the production DHCP/DNS stand on the emulated Intel part.
grep -Fq 'test-production-dhcp-dns-e1000-qemu:' Makefile || {
    echo "ITEM-494: production-проба через Intel не зарегистрирована" >&2
    exit 1
}
# shellcheck disable=SC1003  # the pin is a literal makefile line ending in a backslash
grep -Fq '	test-production-dhcp-dns-e1000-qemu \' Makefile || {
    echo "ITEM-494: production-проба через Intel не в acceptance-наборе" >&2
    exit 1
}
grep -Fq 'HFOC_PRODUCTION_DHCP_DNS_NIC' \
    scripts/test-qemu-production-dhcp-dns.sh || {
    echo "ITEM-494: DHCP/DNS-проба не умеет второй вендор" >&2
    exit 1
}
# shellcheck disable=SC2016  # the pinned script text is literal
grep -Fq -- '-device e1000,netdev=net0,mac="$GUEST_MAC"' \
    scripts/test-qemu-production-dhcp-dns.sh || {
    echo "ITEM-494: DHCP/DNS-проба не грузит Intel-часть" >&2
    exit 1
}
# The proof requires the bound record, not just the driver coming up.
grep -Fq 'driver=E1000 mac=52:54:00:12:34:56 state=AVAILABLE' \
    scripts/test-qemu-production-dhcp-dns.sh || {
    echo "ITEM-494: проба не требует AVAILABLE-записи Intel с адресом части" >&2
    exit 1
}
grep -Fq 'devices=1 supported=[0-9]+ available=1' \
    scripts/test-qemu-production-dhcp-dns.sh || {
    echo "ITEM-494: проба не требует сводки available=1 для Intel" >&2
    exit 1
}
grep -Fq 'backend=E1000' scripts/test-qemu-production-dhcp-dns.sh || {
    echo "ITEM-494: проба не требует backend=E1000 в записи production-сети" >&2
    exit 1
}
# The reading of the machine's controllers already knows the Intel part; this
# item must not have made discovery vendor-specific to do it.
grep -Fq 'driver=E1000' docs/hfoc-nic-discovery.md || {
    echo "ITEM-494: docs/hfoc-nic-discovery.md не называет драйвер E1000" >&2
    exit 1
}
grep -Fq 'QEMU/OVMF proof is not physical hardware acceptance' \
    docs/hfoc-nic-discovery.md || {
    echo "ITEM-494: документ не отделяет QEMU-доказательство от аппаратного" >&2
    exit 1
}
# "The part gave no address" is not six zero bytes: the document states the
# rule the 8254x path applies, and names the production proof that turns the
# record AVAILABLE with the address the driver read.
# shellcheck disable=SC2016  # the pinned document text is literal
grep -Fq 'spelled `mac=NONE`' docs/hfoc-nic-discovery.md || {
    echo "ITEM-494: документ не фиксирует правило «нет адреса — это mac=NONE»" >&2
    exit 1
}
grep -Fq 'e1000_mac_valid' docs/hfoc-nic-discovery.md || {
    echo "ITEM-494: документ не называет проверку адреса у 8254x" >&2
    exit 1
}
grep -Fq 'test-production-dhcp-dns-e1000-qemu' docs/hfoc-nic-discovery.md || {
    echo "ITEM-494: документ не называет production-пробу через Intel" >&2
    exit 1
}
grep -Fq 'state=AVAILABLE' docs/hfoc-nic-discovery.md || {
    echo "ITEM-494: документ не показывает AVAILABLE-запись Intel" >&2
    exit 1
}
