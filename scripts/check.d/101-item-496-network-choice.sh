#!/usr/bin/env bash
# ITEM-496: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The operator's DHCP/static choice is answerable, durable and proven to the
# effect.
#
# ITEM-447 wrote the four questions and the commit record, but with the plain
# character-by-character writer.  The 2026-09-29 reproduction on this tree
# showed what that costs: `[IOAPIC] irq1` trace lines split `gateway: ` in the
# serial capture, so the gesture proof could not find its own marker, and the
# questions and records never reached the durable journal at all.  This slice
# pins the four rules the item restores and the proof that measures the effect:
#
#   * every question and every record is one IRQ-safe write and is mirrored to
#     the journal (the ITEM-440 rule the menu lines already followed);
#   * before the questions the machine publishes what the interface has right
#     now, rendered from the live wire - never from the record the menu is
#     about to replace, and never guessed;
#   * the answer record names both sides of the reboot: the mode just stored
#     and the configuration the interface still runs (`applies=reboot
#     current=`);
#   * the Network entry is offered only when the persistent record is loaded,
#     so a machine that cannot store an answer says `NO_RECORD_OWNER` before
#     asking for one;
#   * the QEMU proof drives a refused answer, an accepted static address that
#     answers ARP and ICMP on the wire, and the return to DHCP with a fresh
#     lease.

for item496_file in \
    kernel/include/local_network_setup.h \
    kernel/src/local_network_setup.c \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/kernel.c \
    tests/local-network-setup-host.c \
    tests/console-host.c \
    scripts/test-qemu-production-install-flow.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/hfoc-production-install-flow.md \
    docs/hfoc-administrator-guide.md \
    docs/hfoc-hardware-acceptance.md; do
    test -f "$item496_file" || {
        echo "ITEM-496: отсутствует файл: $item496_file" >&2
        exit 1
    }
done

# 1. Questions and records are IRQ-safe and durable.  The plain writer is gone
#    from the setup path: a prompt or a record it writes can be split by an IRQ
#    trace line, which is exactly the defect the item removed.
grep -Fq 'console_network_setup_publish_record(' kernel/src/console.c || {
    echo "ITEM-496: записи установки сети не идут через один публикатор" >&2
    exit 1
}
grep -Fq 'console_write_atomic(record);' kernel/src/console.c || {
    echo "ITEM-496: запись установки сети не IRQ-безопасна" >&2
    exit 1
}
grep -Fq 'console_publish_journal(record, length);' kernel/src/console.c || {
    echo "ITEM-496: запись установки сети не попадает в долговечный журнал" >&2
    exit 1
}
grep -Fq 'console_write_atomic(prompt_text);' kernel/src/console.c || {
    echo "ITEM-496: вопрос установки сети не IRQ-безопасен" >&2
    exit 1
}
if grep -Fq 'console_write(prompt_text)' kernel/src/console.c; then
    echo "ITEM-496: посимвольная запись вернулась в вопросы установки сети" >&2
    exit 1
fi

# 2. The current configuration is read out before the first question, from the
#    live wire (the same accessor the boot's own [PRODUCTION-NET-MODE] record
#    reads) - not from the persistent record the menu is about to replace.
grep -Fq 'local_network_setup_offer(' kernel/include/local_network_setup.h \
    kernel/src/local_network_setup.c kernel/src/console.c || {
    echo "ITEM-496: запись о текущем режиме перед вопросами не публикуется" >&2
    exit 1
}
grep -Fq '"[PRODUCTION-NET-SET] state=OFFERED current="' \
    kernel/src/local_network_setup.c || {
    echo "ITEM-496: запись OFFERED не называет текущий режим" >&2
    exit 1
}
grep -Fq 'appliance_network_wire_active_config(wire)' kernel/src/kernel.c || {
    echo "ITEM-496: текущий режим не читается с живого интерфейса" >&2
    exit 1
}
grep -Fq 'appliance_production_network_wire_boot_mode()' kernel/src/kernel.c || {
    echo "ITEM-496: текущий режим не берётся у wire-владельца" >&2
    exit 1
}
grep -Fq 'active->address.octets[octet]' kernel/src/kernel.c || {
    echo "ITEM-496: текущий статический адрес не рендерится" >&2
    exit 1
}

# 3. The answer record names both what was stored and what still runs.
grep -Fq '" applies=reboot current="' kernel/src/local_network_setup.c || {
    echo "ITEM-496: запись ответа не называет, что осталось на интерфейсе" >&2
    exit 1
}
grep -Fq 'console_network_setup_current_text(' kernel/src/console.c || {
    echo "ITEM-496: консоль не передаёт текущий режим в запись ответа" >&2
    exit 1
}

# 4. The entry is bound only to a loaded record, so an appliance that cannot
#    store an answer refuses before the questions instead of after them - and
#    the entry names that refusal in so many words.
grep -Fq 'appliance_persistent_network_config_current(network_store) !=' \
    kernel/src/kernel.c || {
    echo "ITEM-496: пункт меню связывается без загруженной записи" >&2
    exit 1
}
grep -Fq 'console_bind_network_config(NULL);' kernel/src/kernel.c || {
    echo "ITEM-496: незагруженная запись не отказывает пункту меню" >&2
    exit 1
}
grep -Fq 'state=UNAVAILABLE reason=NO_RECORD_OWNER' \
    kernel/src/local_network_setup.c || {
    echo "ITEM-496: отказ без владельца записи не назван" >&2
    exit 1
}
grep -Fq 'local_network_setup_unavailable(' kernel/src/console.c || {
    echo "ITEM-496: пункт меню не публикует отказ без владельца записи" >&2
    exit 1
}
grep -Fq 'without a record owner the entry publishes the reason' \
    tests/console-host.c || {
    echo "ITEM-496: host-проба не пиннит отказ без владельца записи" >&2
    exit 1
}

# 5. The host proofs pin the atomic questions, the offer record and the honest
#    UNAVAILABLE reading.
grep -Fq 'state=OFFERED current=DHCP' tests/console-host.c || {
    echo "ITEM-496: host-проба не читает запись OFFERED" >&2
    exit 1
}
grep -Fq 'current=UNAVAILABLE' tests/console-host.c \
    tests/local-network-setup-host.c || {
    echo "ITEM-496: host-пробы не пиннят честное UNAVAILABLE" >&2
    exit 1
}
grep -Fq 'serial_contains_atomic(' tests/console-host.c || {
    echo "ITEM-496: вопросы не проверены как одна IRQ-безопасная запись" >&2
    exit 1
}
grep -Fq 'local_network_setup_offer(' tests/local-network-setup-host.c || {
    echo "ITEM-496: модуль не пиннут на запись OFFERED" >&2
    exit 1
}

# 6. The QEMU proof measures the effect, not the record: all three answers on
#    an installed disk, and the static address has to answer the host's own
#    ARP and ICMP on a raw socket backend.
grep -Fq 'probe_static_address(network_address)' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-496: проба не проверяет, что статический адрес отвечает" >&2
    exit 1
}
grep -Fq 'build_probe_arp_request(' scripts/test-qemu-production-install-flow.sh ||
    grep -Fq 'build_probe_icmp_request(' scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-496: у пробы нет ARP/ICMP-пира" >&2
    exit 1
}
grep -Fq 'bad-address' scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-496: проба не ведёт отказ на негодном адресе" >&2
    exit 1
}
grep -Fq 'reason=NETWORK_IPV4_INVALID_PREFIX' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-496: проба не называет причину отказа" >&2
    exit 1
}
grep -Fq 'expect-dhcp' scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-496: проба не проверяет возврат на DHCP" >&2
    exit 1
}
grep -Fq 'lease=ACQUIRED address=' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-496: проба не читает аренду после возврата на DHCP" >&2
    exit 1
}

# 7. The documents tell the operator what he sees and when it applies.
grep -Fq 'state=OFFERED current=' docs/hfoc-appliance-boot-console.md \
    docs/hfoc-administrator-guide.md docs/hfoc-production-install-flow.md || {
    echo "ITEM-496: документы не описывают чтение текущего режима" >&2
    exit 1
}
grep -Fq 'NETWORK_CHOICE' docs/hfoc-hardware-acceptance.md \
    docs/ROADMAP.md || {
    echo "ITEM-496: рунбук не описывает стадию выбора сети" >&2
    exit 1
}
