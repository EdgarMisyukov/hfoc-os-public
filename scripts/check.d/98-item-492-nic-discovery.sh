#!/usr/bin/env bash
# ITEM-492: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The machine's network controllers, discovered and described - not assumed.
#
# Before this item the only network question the product could answer was "is
# there a Realtek part?", asked inside the RTL8168 driver's own file, so every
# other controller and a machine with no controller produced the same silence.
# This slice pins the rules that make the description a property of the machine:
#
#   * discovery walks the PCI bus for the network class with the subclass left
#     open and names no vendor - a Realtek part and a part nobody wrote a driver
#     for take the same path, and the difference is the state, not a branch;
#   * a table that cannot hold the machine is refused whole, never truncated: a
#     truncated list is a machine misdescribed;
#   * the claims live in one registry that refuses two backends claiming the
#     same ids, and the RTL8168 claim is unchanged by this item;
#   * a capability reading never claims an initialised device: the installer's
#     scan runs without a bind callback, so `available=` stays zero and a driver
#     in the image is not a device that works;
#   * the reading is published on every boot and by the install flow, and the
#     install states its own rule - `install=CONTINUES` - rather than leaving it
#     to be inferred;
#   * the proof that no controller reads `devices=0 ... absent=1` runs on a
#     machine that really has no controller: QEMU gives a default NIC unless it
#     is told not to, and a default NIC would have made that reading untested.

for item492_file in \
    kernel/include/nic_registry.h \
    kernel/src/nic_registry.c \
    kernel/include/nic_discovery.h \
    kernel/src/nic_discovery.c \
    kernel/include/nic_backends.h \
    kernel/src/nic_backends.c \
    kernel/include/pci_io.h \
    kernel/src/pci_io.c \
    tests/nic-registry-host.c \
    tests/nic-discovery-host.c \
    scripts/test-qemu-nic-discovery.sh \
    docs/hfoc-nic-discovery.md; do
    test -f "$item492_file" || {
        echo "ITEM-492: отсутствует файл: $item492_file" >&2
        exit 1
    }
done

# ------------------------------------------------- discovery knows no vendor --
for item492_vendor in rtl8168 RTL8168 virtio VIRTIO 10EC 1AF4 8168; do
    if grep -Fq "$item492_vendor" kernel/src/nic_discovery.c; then
        echo "ITEM-492: обнаружение знает про $item492_vendor и снова спрашивает драйвер" >&2
        exit 1
    fi
done
# It asks the bus for the network class, with the subclass left open.
grep -Fq 'PCI_CLASS_NETWORK' kernel/src/nic_discovery.c || {
    echo "ITEM-492: обнаружение не ищет класс сети" >&2
    exit 1
}
grep -Fq 'PCI_FILTER_ANY' kernel/src/nic_discovery.c || {
    echo "ITEM-492: обнаружение сузило подкласс сети" >&2
    exit 1
}
# A machine the table cannot hold is refused, not described in part.
grep -Fq 'NIC_DISCOVERY_CAPACITY' kernel/src/nic_discovery.c || {
    echo "ITEM-492: переполнение таблицы NIC не отвергается" >&2
    exit 1
}

# ------------------------------------------ one list of claims, no ambiguity --
for item492_backend in NIC_BACKEND_NAME_RTL8168 NIC_BACKEND_NAME_VIRTIO; do
    grep -Fq "$item492_backend" kernel/src/nic_backends.c || {
        echo "ITEM-492: в списке backend'ов нет $item492_backend" >&2
        exit 1
    }
done
grep -Fq 'nic_registry_register' kernel/src/nic_backends.c || {
    echo "ITEM-492: backend'ы не регистрируются в реестре" >&2
    exit 1
}
# The registry refuses a claim it already holds instead of resolving it by order.
grep -Fq 'nic_registry_match' kernel/src/nic_registry.c || {
    echo "ITEM-492: реестр NIC не сопоставляет claim'ы" >&2
    exit 1
}

# ------------------------------------------------ the reading is on every boot --
grep -Fq 'kernel_publish_nic_capabilities' kernel/src/kernel.c || {
    echo "ITEM-492: ядро не публикует чтение NIC при загрузке" >&2
    exit 1
}
grep -Fq 'nic_summary_record' kernel/src/kernel.c || {
    echo "ITEM-492: ядро не публикует сводку NIC" >&2
    exit 1
}
grep -Fq '[INSTALLER-NIC] devices=' kernel/src/production_install_flow.c || {
    echo "ITEM-492: поток установки не публикует чтение NIC" >&2
    exit 1
}
grep -Fq 'install=CONTINUES' kernel/src/production_install_flow.c || {
    echo "ITEM-492: установка не утверждает, что продолжается без сети" >&2
    exit 1
}

# ------------------------------------------- the proofs are registered, honestly --
grep -Fq 'test-nic-registry-host' Makefile || {
    echo "ITEM-492: host-проба реестра NIC не зарегистрирована" >&2
    exit 1
}
grep -Fq 'test-nic-discovery-host' Makefile || {
    echo "ITEM-492: host-проба обнаружения NIC не зарегистрирована" >&2
    exit 1
}
grep -Fq 'test-nic-discovery-qemu' Makefile || {
    echo "ITEM-492: QEMU-проба обнаружения NIC не зарегистрирована" >&2
    exit 1
}
# shellcheck disable=SC2016  # the pinned Makefile text is literal
grep -Fq '$(MAKE) test-nic-registry-host' Makefile || {
    echo "ITEM-492: host-проба реестра NIC не в каноническом host-suite" >&2
    exit 1
}
# shellcheck disable=SC2016  # the pinned Makefile text is literal
grep -Fq '$(MAKE) test-nic-discovery-host' Makefile || {
    echo "ITEM-492: host-проба обнаружения NIC не в каноническом host-suite" >&2
    exit 1
}
# "No controller at all" must be a machine with no controller.
grep -Fq -- '-nic none' scripts/test-qemu-nic-discovery.sh || {
    echo "ITEM-492: проба «нет NIC» не отключает NIC по умолчанию" >&2
    exit 1
}
# A capability reading must never be allowed to claim an initialised device.
grep -Fq 'available=0' scripts/test-qemu-nic-discovery.sh || {
    echo "ITEM-492: проба NIC не запрещает available= в чтении способностей" >&2
    exit 1
}

# ------------------------------------------------- the document says the same --
for item492_doc_word in \
    'UNSUPPORTED' \
    'ABSENT' \
    'install=CONTINUES' \
    'QEMU/OVMF proof is not physical hardware acceptance'; do
    grep -Fq -- "$item492_doc_word" docs/hfoc-nic-discovery.md || {
        echo "ITEM-492: docs/hfoc-nic-discovery.md не фиксирует ITEM-492: $item492_doc_word" >&2
        exit 1
    }
done
